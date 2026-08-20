#include "vm.h"
#include "list.h"

/*
    RWList: shared reader/writer access to a CONCURRENT_LIST_HEAD.

    The modified and standby lists are hit by many threads at once and cannot be sharded
    (ordering carries meaning), so they use a hybrid: an SRWLOCK on the list plus a
    CRITICAL_SECTION embedded in every PFN, plus a dedicated lock (head_lock) for the head
    sentinel, which is not a PFN and so has no lock of its own.

    Everyone enters shared and locks individual nodes hand-over-hand, so several threads can
    insert at the tail, remove from the middle, and scan at once as long as they are not
    touching adjacent nodes. Two policies sit on top of that:

      "I want THIS exact node" (RWListInsertTail / RWListRemoveKnown): cannot skip a contended
      neighbor, so it retries LIST_COUPLE_MAX_FAILS times then escalates to the exclusive lock,
      which guarantees completion. Every critical section is released before escalating so the
      escalation can never be the tail of a wait cycle.

      "I want ANY eligible node" (RWListScanBegin/Next/RemoveCurrent/End): never escalates. A
      contended node just ends the pass and is picked up on a later one. The cursor holds pred
      and curr; a successful remove transfers ownership of the removed node's lock to the caller
      (mirrors lock_pfn / unlock_pfn).

    INVARIANT: a node's Flink/Blink may only be written while holding that node's lock. So an
    insert holds the two straddling nodes' locks, and a remove holds the node's lock plus both
    neighbors' locks. head_lock stands in for the sentinel's lock throughout.
*/

// Resolve the lock guarding a list entry: the head sentinel uses the list's head_lock,
// every real node uses its own embedded pfn->lock.
static CRITICAL_SECTION*
rwlist_node_lock(PCONCURRENT_LIST_HEAD L, PLIST_ENTRY entry)
{
    if (entry == &L->entry) return &L->head_lock;
    return &get_pfn_from_PListEntry(entry)->lock;
}

// Insert node at the tail under shared access.
// PRECONDITION: caller holds node's lock and node is unlinked (links are NULL).
VOID
RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    AcquireSRWLockShared(&L->srw);
    ULONG64 fails = 0;

    for (;;) {
        // head_lock is the entry point for every tail insert.
        if (!TryEnterCriticalSection(&L->head_lock)) {
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;   // nothing held on this branch, safe to escalate straight away
        }

        // On an empty list head.Blink IS the sentinel, so tail_lock collapses to head_lock;
        // same_lock stops us double-acquiring the critical section we already hold.
        PLIST_ENTRY tail = L->entry.Blink;
        CRITICAL_SECTION* tail_lock = rwlist_node_lock(L, tail);
        BOOLEAN same_lock = (tail_lock == &L->head_lock);
        if (!same_lock && !TryEnterCriticalSection(tail_lock)) {
            LeaveCriticalSection(&L->head_lock);
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }

        // Both neighbors of the new node locked: splice in at the tail.
        node->links.Flink = &L->entry;
        node->links.Blink = tail;
        tail->Flink = &node->links;
        L->entry.Blink = &node->links;
        InterlockedIncrement64(&L->list_count);

        if (!same_lock) LeaveCriticalSection(tail_lock);
        LeaveCriticalSection(&L->head_lock);
        ReleaseSRWLockShared(&L->srw);
        return;
    }

escalate:
    // Every critical section from the loop is released by now, so blocking on exclusive
    // (which waits for all shared holders to leave) cannot close a wait cycle.
    ReleaseSRWLockShared(&L->srw);
    AcquireSRWLockExclusive(&L->srw);
    InsertTailList(&L->entry, &node->links);   // exclusive already excludes everyone else
    InterlockedIncrement64(&L->list_count);
    ReleaseSRWLockExclusive(&L->srw);
}

// Unlink a SPECIFIC node the caller already knows about (e.g. a soft fault poaching one
// exact frame back off modified/standby).
// PRECONDITION: caller holds node's lock for the whole call (including escalation), which
// is what makes reading node->links.Flink/Blink safe without extra locking.
VOID
RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
#if DEBUG
    {
        // Node's recorded list type should match the list it is being removed from.
        ULONG64 expect = (L == &standbyList_head) ? LIST_STANDBY : LIST_MODIFIED;
        if (node->list_type != expect) {
            printf("WRONG-LIST REMOVE: node=%p frame=%llu list_type=%llu removing-from=%s\n",
                node, node->frame_number, (ULONG64)node->list_type,
                (L == &standbyList_head) ? "standby" : "modified");
            DebugBreak();
        }
    }
#endif

    AcquireSRWLockShared(&L->srw);
    ULONG64 fails = 0;

    for (;;) {
        PLIST_ENTRY entry = &node->links;
        if (entry->Flink == NULL) {   // already off the list (both links NULL)
            ReleaseSRWLockShared(&L->srw);
            return;
        }
        // node's links are stable because the caller holds node's lock, so re-read fresh.
        PLIST_ENTRY succ = entry->Flink;
        PLIST_ENTRY pred = entry->Blink;
        CRITICAL_SECTION* succ_lock = rwlist_node_lock(L, succ);
        CRITICAL_SECTION* pred_lock = rwlist_node_lock(L, pred);

        if (!TryEnterCriticalSection(succ_lock)) {
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }
        // pred and succ can resolve to the same lock (node is the only real entry).
        BOOLEAN same_lock = (pred_lock == succ_lock);
        if (!same_lock && !TryEnterCriticalSection(pred_lock)) {
            LeaveCriticalSection(succ_lock);
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }

        // All three locks held (node's via the caller, succ's and pred's just now).
        pred->Flink = succ;
        succ->Blink = pred;
        entry->Flink = entry->Blink = NULL;
        InterlockedDecrement64(&L->list_count);

        if (!same_lock) LeaveCriticalSection(pred_lock);
        LeaveCriticalSection(succ_lock);
        ReleaseSRWLockShared(&L->srw);
        return;
    }

escalate:
    ReleaseSRWLockShared(&L->srw);
    AcquireSRWLockExclusive(&L->srw);
    if (node->links.Flink != NULL) {
        RemoveEntryList(&node->links);
        node->links.Flink = node->links.Blink = NULL;
        InterlockedDecrement64(&L->list_count);
    }
    ReleaseSRWLockExclusive(&L->srw);
}

// ---------------------------------------------------------------------------------------
// RWListScanBegin/Next/RemoveCurrent/End: Policy-B "any eligible node" walk under shared
// access. A contended node ends the pass (returns NULL) rather than escalating; the caller
// re-scans later to pick it up. The cursor always holds pred (last node confirmed linked)
// and curr (node most recently returned); advancing promotes curr into pred's place so a
// later RWListScanRemoveCurrent still has pred to splice around whatever comes next.

// Begin a walk. forward=TRUE walks head->tail (Flink), FALSE walks tail->head (Blink).
// Returns the first eligible node (with its lock held) or NULL. The blocking wait on
// head_lock is the only blocking acquire in the whole scan path (everything else is a
// TryEnter), and its holder never blocks while holding it, so it cannot close a cycle.
pfn_metadata*
RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward)
{
    AcquireSRWLockShared(&L->srw);
    EnterCriticalSection(&L->head_lock);
    c->list = L;
    c->pred = &L->entry;
    c->pred_lock = &L->head_lock;
    c->curr = NULL;
    c->curr_lock = NULL;
    c->forward = forward;
    return RWListScanNext(c);
}

// Advance one node. Returns the newly-visited node with its OWN lock held (caller must
// eventually release it, directly or via a successful RWListScanRemoveCurrent handoff), or
// NULL for either "list exhausted" or "next node contended" -- both mean "stop this pass".
pfn_metadata*
RWListScanNext(RW_LIST_CURSOR* c)
{
    PLIST_ENTRY from = (c->curr != NULL) ? c->curr : c->pred;   // both are lock-held
    PLIST_ENTRY next = c->forward ? from->Flink : from->Blink;

    if (next == &c->list->entry) {
        // Exhausted. If standing on a real node, promote it to pred (drop old pred) so the
        // cursor is in a well-defined state and RWListScanEnd releases exactly once.
        if (c->curr != NULL) {
            LeaveCriticalSection(c->pred_lock);
            c->pred = c->curr;
            c->pred_lock = c->curr_lock;
            c->curr = NULL;
            c->curr_lock = NULL;
        }
        return NULL;
    }

    CRITICAL_SECTION* next_lock = &get_pfn_from_PListEntry(next)->lock;
    if (!TryEnterCriticalSection(next_lock)) return NULL;   // contended: stop this pass

#if DEBUG
    {
        ULONG64 lt = get_pfn_from_PListEntry(next)->list_type;
        ULONG64 expect = (c->list == &standbyList_head) ? LIST_STANDBY : LIST_MODIFIED;
        if (lt != expect) {
            printf("SCAN WRONG-TYPE NODE (%s): node %p list_type=%llu expected %llu\n",
                (c->list == &standbyList_head) ? "standby" : "modified", next, lt, expect);
            DebugBreak();
        }
    }
#endif

    // Advanced: old curr becomes pred (lock ownership transfers, not released) so it stays
    // available to splice if the caller removes whatever comes next.
    if (c->curr != NULL) {
        LeaveCriticalSection(c->pred_lock);
        c->pred = c->curr;
        c->pred_lock = c->curr_lock;
    }
    c->curr = next;
    c->curr_lock = next_lock;
    return get_pfn_from_PListEntry(next);
}

// Unlink the node last returned by RWListScanNext. Needs three locks: pred (held), curr
// (held), and curr's far neighbor (acquired here; same_lock covers a 2-real-node list where
// it is pred). On success: splices curr out, transfers ownership of curr's lock TO THE
// CALLER (who must unlock_pfn it), leaves pred positioned so the next ScanNext continues
// seamlessly, and returns the removed node. On failure (far neighbor contended): nothing
// changes, curr stays linked and cursor-held, returns NULL -- caller should ScanNext past it.
pfn_metadata*
RWListScanRemoveCurrent(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->curr == NULL) return NULL;

    PLIST_ENTRY succ = c->forward ? c->curr->Flink : c->curr->Blink;
    CRITICAL_SECTION* succ_lock = rwlist_node_lock(L, succ);
    BOOLEAN same_lock = (succ_lock == c->pred_lock);

    if (!same_lock && !TryEnterCriticalSection(succ_lock)) return NULL;

    pfn_metadata* removed = get_pfn_from_PListEntry(c->curr);

    if (c->forward) {
        c->pred->Flink = succ;
        succ->Blink = c->pred;
    } else {
        c->pred->Blink = succ;
        succ->Flink = c->pred;
    }
    c->curr->Flink = c->curr->Blink = NULL;
    InterlockedDecrement64(&L->list_count);

    if (!same_lock) LeaveCriticalSection(succ_lock);
    // curr's lock ownership transfers to the caller: deliberately NOT released here.
    c->curr = NULL;
    c->curr_lock = NULL;
    return removed;
}

// End a scan: release curr (NULL if a successful RWListScanRemoveCurrent already handed its
// lock to the caller), release pred, and drop the shared SRW hold.
VOID
RWListScanEnd(RW_LIST_CURSOR* c)
{
    if (c->curr_lock != NULL) LeaveCriticalSection(c->curr_lock);
    LeaveCriticalSection(c->pred_lock);
    ReleaseSRWLockShared(&c->list->srw);
}

// Rescue the current standby node by repointing its owner transition PTE to disc, then
// unlink it and transfer its lock to the caller. Like RWListScanRemoveCurrent, it needs
// curr's far neighbor lock; nothing is mutated until both that lock and the rescue succeed,
// so on contention the node is left fully intact on standby. Returns the pfn or NULL.
pfn_metadata*
rwlist_scan_rescue_remove(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->curr == NULL) return NULL;

    PLIST_ENTRY succ = c->forward ? c->curr->Flink : c->curr->Blink;
    CRITICAL_SECTION* succ_lock = rwlist_node_lock(L, succ);
    BOOLEAN same_lock = (succ_lock == c->pred_lock);

    if (!same_lock && !TryEnterCriticalSection(succ_lock)) return NULL;

    pfn_metadata* pfn = get_pfn_from_PListEntry(c->curr);

    // Commit the PTE repoint before touching the list. If the owner region is contended,
    // release the far-neighbor lock and leave the node fully intact on standby.
    if (!rescue_standby_to_disc(pfn)) {
        if (!same_lock) LeaveCriticalSection(succ_lock);
        return NULL;
    }

    if (c->forward) {
        c->pred->Flink = succ;
        succ->Blink = c->pred;
    } else {
        c->pred->Blink = succ;
        succ->Flink = c->pred;
    }
    c->curr->Flink = c->curr->Blink = NULL;
    InterlockedDecrement64(&L->list_count);

    claim_rescued_pfn(pfn);   // list_type -> NONE, take ownership

    if (!same_lock) LeaveCriticalSection(succ_lock);
    c->curr = NULL;
    c->curr_lock = NULL;   // transfer curr's lock to the caller
    ASSERT(pfn->links.Flink == NULL && pfn->links.Blink == NULL);
    return pfn;
}

// Walk concurrent list under exclusive srw and DebugBreak on the first inconsistency
// Checks that every node is a real frame inside physical_slots, doubly-linked consistency, 
// list_type matches the list, no runaway/cycle, walked count matches list_count
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
        // Wild-pointer guard: real node's links live inside physical_slots
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
    if (count == lc || count > cap) { /* count mismatch is only meaningful on a clean full walk */ }
    else {
        // Auto-correct list_count to true count and keep running
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
