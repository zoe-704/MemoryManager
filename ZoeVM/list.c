#include "vm.h"
#include "list.h"

// ===========================================================================
// RWList: reader/writer access to a CONCURRENT_LIST_HEAD (Option 1: exclusive writers).
//
//   * The list's SRW gives parallelism: SHARED = read-only walkers, EXCLUSIVE = ANY structural
//     mutation (insert / remove / scan+remove). A mutation under EXCLUSIVE is the SOLE list
//     mutator, so every splice is a plain doubly-linked-list operation -- no per-node
//     lock-coupling, no neighbor locks, no escalation. That makes torn nodes / stale tails /
//     orphaned segments structurally impossible.
//   * Scans still take each visited node's pfn lock via TryEnter (skip-on-contention) so
//     callers get the frame locked to read/mutate its state. TryEnter (never block) is
//     required for deadlock-freedom: an inserter/remover holds a pfn lock and then wants our
//     EXCLUSIVE srw, so blocking on that pfn lock would deadlock. Skipping is safe -- the ring
//     is stable under EXCLUSIVE and the blocked remover completes the instant we end the scan.
//   * list_type (under the pfn lock) stays the authoritative "which list" answer.
//
// The RW_LIST_CURSOR uses only `cur` (current node, or the anchor) and `cur_lock` (the pfn
// lock we currently hold, or NULL); the prev/prev_lock fields are unused in this model.
// ===========================================================================

// Insert node at the tail. Caller holds node->lock; node->links must be the NULL sentinel.
VOID
RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    AcquireSRWLockExclusive(&L->srw);
    InsertTailList(&L->entry, &node->links);   // sole mutator: plain splice
    InterlockedIncrement64(&L->list_count);
    ReleaseSRWLockExclusive(&L->srw);
}

// Remove a node the caller already owns (holds node->lock). No-op if already off the list.
VOID
RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    // WRONG-LIST GUARD: splicing `node` out of L uses node's own links, valid only if node is
    // physically ON L. The caller picks L from node->list_type (under node->lock), so with
    // coherent state list_type always names L. If not, refuse rather than corrupt L.
    {
        ULONG64 expect = (L == &standbyList_head) ? LIST_STANDBY : LIST_MODIFIED;
        if (node->list_type != expect) {
#if DEBUG
            printf("WRONG-LIST REMOVE: node=%p frame=%llu list_type=%llu removing-from=%s\n",
                node, node->frame_number, (ULONG64)node->list_type,
                (L == &standbyList_head) ? "standby" : "modified");
            DebugBreak();
#endif
            return;
        }
    }

    AcquireSRWLockExclusive(&L->srw);
    if (node->links.Flink != NULL) {   // on the list (both links set); sentinel is both NULL
        RemoveEntryList(&node->links);
        node->links.Flink = NULL;
        node->links.Blink = NULL;
        InterlockedDecrement64(&L->list_count);
    }
    ReleaseSRWLockExclusive(&L->srw);
}

// Advance to the next lockable real node, skipping contended ones. Under EXCLUSIVE the ring is
// stable, so no neighbor locks are needed -- just TryEnter the single current node's pfn lock.
// Returns the node (with its lock held, transferred to the caller's care) or NULL at end.
static pfn_metadata*
rwlist_scan_advance(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;

    // Release the node returned by the previous advance (unless a scan-remove already cleared
    // cur_lock by transferring the lock to the caller).
    if (c->cur_lock != NULL) {
        LeaveCriticalSection(c->cur_lock);
        c->cur_lock = NULL;
    }

    PLIST_ENTRY e = c->cur;
    for (;;) {
        PLIST_ENTRY next = c->forward ? e->Flink : e->Blink;
        if (next == &L->entry) {
            c->cur = &L->entry;   // wrapped to the anchor: end of list
            return NULL;
        }
#if DEBUG
        if ((BYTE*)next < (BYTE*)physical_slots ||
            (BYTE*)next >(BYTE*)&physical_slots[max_frame_number]) {
            printf("SCAN WILD NODE (%s): from %p -> next %p\n",
                (L == &standbyList_head) ? "standby" : "modified", e, next);
            DebugBreak();
            c->cur = &L->entry;
            return NULL;
        }
#endif
        CRITICAL_SECTION* nl = &get_pfn_from_PListEntry(next)->lock;
        if (TryEnterCriticalSection(nl)) {
            c->cur = next;
            c->cur_lock = nl;
#if DEBUG
            {
                ULONG64 lt = get_pfn_from_PListEntry(next)->list_type;
                ULONG64 expect = (L == &standbyList_head) ? LIST_STANDBY : LIST_MODIFIED;
                if (lt != expect) {
                    printf("SCAN WRONG-TYPE NODE (%s): node %p list_type=%llu expected %llu\n",
                        (L == &standbyList_head) ? "standby" : "modified", next, lt, expect);
                    DebugBreak();
                }
            }
#endif
            return get_pfn_from_PListEntry(next);
        }
        // Contended: the holder is a remover/inserter blocked on our exclusive srw. Skip past
        // this node (ring is stable under EXCLUSIVE) and keep looking; it is picked up next pass.
        e = next;
    }
}

// Begin a scan under EXCLUSIVE srw; return the first lockable real node (NULL if empty).
pfn_metadata*
RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward)
{
    AcquireSRWLockExclusive(&L->srw);
    c->list = L;
    c->forward = forward;
    c->cur = &L->entry;
    c->cur_lock = NULL;
    c->prev = NULL;         // unused in the exclusive model
    c->prev_lock = NULL;
    return rwlist_scan_advance(c);
}

// Advance to and return the next node (with its lock held), or NULL at end of list.
pfn_metadata*
RWListScanNext(RW_LIST_CURSOR* c)
{
    return rwlist_scan_advance(c);
}

// Remove the current node and TRANSFER its lock to the caller. Under EXCLUSIVE this always
// succeeds (sole mutator). The cursor resumes from cur's predecessor.
pfn_metadata*
RWListScanRemoveCurrent(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->cur == &L->entry) return NULL;   // not positioned on a real node

    PLIST_ENTRY cur = c->cur;
    PLIST_ENTRY resume = c->forward ? cur->Blink : cur->Flink;   // predecessor to continue from

    RemoveEntryList(cur);   // sole mutator: plain splice
    cur->Flink = NULL;
    cur->Blink = NULL;
    InterlockedDecrement64(&L->list_count);

    pfn_metadata* removed = get_pfn_from_PListEntry(cur);

    // Transfer cur's lock to the caller; cursor resumes from the (unlocked) predecessor.
    c->cur = resume;
    c->cur_lock = NULL;
    return removed;
}

// End a scan: release the current node's lock (if still held) and the EXCLUSIVE srw.
VOID
RWListScanEnd(RW_LIST_CURSOR* c)
{
    if (c->cur_lock != NULL) {
        LeaveCriticalSection(c->cur_lock);
        c->cur_lock = NULL;
    }
    ReleaseSRWLockExclusive(&c->list->srw);
}

// Rescue the current standby node (repoint its owner PTE to disc) then remove it, transferring
// its lock to the caller. Returns NULL (leaving the node on standby, cursor intact) if the
// owner region is contended. Under EXCLUSIVE the unlink itself always succeeds.
pfn_metadata*
rwlist_scan_rescue_remove(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->cur == &L->entry) return NULL;

    PLIST_ENTRY cur = c->cur;
    pfn_metadata* pfn = get_pfn_from_PListEntry(cur);

    if (!rescue_standby_to_disc(pfn)) {
        return NULL;   // owner region contended: leave the node on standby (cursor intact)
    }

    PLIST_ENTRY resume = c->forward ? cur->Blink : cur->Flink;
    RemoveEntryList(cur);
    cur->Flink = NULL;
    cur->Blink = NULL;
    InterlockedDecrement64(&L->list_count);

    claim_rescued_pfn(pfn);   // list_type = NONE (stamp owner)

    c->cur = resume;
    c->cur_lock = NULL;   // transfer cur's lock to the caller
    ASSERT(pfn->links.Flink == NULL && pfn->links.Blink == NULL);
    return pfn;
}

// Debug integrity checker: walk a concurrent list under EXCLUSIVE srw (a clean, mutation-free
// snapshot) and DebugBreak on the FIRST inconsistency -- so corruption is caught within ~1s of
// being introduced, with the list still intact, instead of minutes later as a wild pointer.
// Checks: every node is a real frame (inside physical_slots), doubly-linked consistency
// (node->Blink == prev), list_type matches the list (never ACTIVE), no runaway/cycle, and the
// walked count matches list_count. Call from periodic_thread.
VOID
check_concurrent_list(PCONCURRENT_LIST_HEAD L, ULONG64 expected_type, const char* name)
{
#if DEBUG
    AcquireSRWLockExclusive(&L->srw);
    ULONG64 count = 0;
    PLIST_ENTRY prev = &L->entry;
    PLIST_ENTRY e = L->entry.Flink;
    ULONG64 cap = (ULONG64)max_frame_number + 8;   // runaway/cycle guard

    while (e != &L->entry) {
        // Wild-pointer guard FIRST -- a real node's links live inside physical_slots
        // (links is at offset 0, so the entry pointer IS the pfn pointer). Catch a bad
        // Flink here, before we dereference it.
        if ((BYTE*)e < (BYTE*)physical_slots ||
            (BYTE*)e >(BYTE*)&physical_slots[max_frame_number]) {
            printf("LIST CORRUPT (%s): wild node %p after prev %p at count %llu\n",
                name, e, prev, count);
            DebugBreak();
            break;
        }
        pfn_metadata* pfn = get_pfn_from_PListEntry(e);

        if (e->Blink != prev) {
            printf("LIST CORRUPT (%s): node %p Blink=%p != prev %p at count %llu\n",
                name, e, e->Blink, prev, count);
            DebugBreak();
            break;
        }
        ULONG64 lt = pfn->list_type;
        if (lt != expected_type) {
            printf("LIST CORRUPT (%s): node %p list_type=%llu (expected %llu) Flink=%p Blink=%p at count %llu\n",
                name, pfn, lt, expected_type, e->Flink, e->Blink, count);
            DebugBreak();
            break;
        }
        if (++count > cap) {
            printf("LIST CORRUPT (%s): walk exceeded pool (cycle?) at count %llu\n", name, count);
            DebugBreak();
            break;
        }
        prev = e;
        e = e->Flink;
    }

    ULONG64 lc = (ULONG64)InterlockedOr64((volatile LONG64*)&L->list_count, 0);
    if (count == lc || count > cap) { /* count mismatch only meaningful on a clean full walk */ }
    else {
        // Count drift is the MILD signal (an unpaired increment/decrement) -- the structure
        // above is intact. Don't break on it: log, auto-correct list_count to the physical
        // truth, and keep running so the FATAL structural checks (wild node / Blink break /
        // active-in-list) can catch the dangerous corruption with a clean dump.
        printf("LIST DRIFT (%s): walked %llu but list_count=%llu -- correcting, continuing\n",
            name, count, lc);
        InterlockedExchange64((volatile LONG64*)&L->list_count, (LONG64)count);
    }
    ReleaseSRWLockExclusive(&L->srw);
#else
    (void)L; (void)expected_type; (void)name;
#endif
}


// Functions for Linked Lists
VOID
InitializeListHead(PLIST_ENTRY ListHead)
{
    ListHead->Flink = ListHead->Blink = ListHead;
    return;
}

VOID
InitializeList(PLIST_HEAD Head)
{
    InitializeListHead(&Head->entry);
    InitializeCriticalSectionAndSpinCount(&Head->list_lock, 0x00FFFFFF);
    Head->list_count = 0;
    return;
}

VOID
InitializeConcurrentList(PCONCURRENT_LIST_HEAD Head)
{
    InitializeListHead(&Head->entry);
    InitializeSRWLock(&Head->srw);
    InitializeCriticalSectionAndSpinCount(&Head->head_lock, 0x00FFFFFF);
    Head->list_count = 0;
    return;
}


BOOLEAN
IsListEmpty(PLIST_ENTRY ListHead)
{
    return (BOOLEAN)(ListHead->Flink == ListHead);
}

VOID
InsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry)
{
    PLIST_ENTRY Flink;

    // Insert a new entry at the head.
    Flink = ListHead->Flink;

    Entry->Flink = Flink;
    Entry->Blink = ListHead;

    Flink->Blink = Entry;

    ListHead->Flink = Entry;

    return;
}


VOID
InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry)
{
    PLIST_ENTRY Blink;

    // Insert a new entry at the tail.
    Blink = ListHead->Blink;

    Entry->Flink = ListHead;
    Entry->Blink = Blink;

    Blink->Flink = Entry;

    ListHead->Blink = Entry;

    return;
}

PLIST_ENTRY
RemoveHeadList(PLIST_ENTRY ListHead)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    // Remove the entry currently at the head of the list.
    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

BOOLEAN
RemoveEntryList(PLIST_ENTRY Entry)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    // Remove the caller's known entry.
    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    // Return whether list is now empty.
    return (BOOLEAN)(Flink == Blink);
}
