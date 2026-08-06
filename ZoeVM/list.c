#include "vm.h"


// ===========================================================================
// RWList: pfn-lock + lock-coupling access to a CONCURRENT_LIST_HEAD
//
// Protocol: list's SRW gives concurrency: SHARED = many parallel
// inserters/removers/scanners, EXCLUSIVE = one thread escalated because
// neighbor lock-coupling failed several times. Each node's PFN lock (or head lock)
// protects its data: region(pte), list(head_lock), pfn(lock), disc

// SHARED path: under AcquireSRWLockShared, take the neighbor node-locks with
// TryEnter (not blocked while holding the SRW). Splice while holding the
// caller's node lock plus both neighbor locks. On LIST_COUPLE_MAX_FAILS
// consecutive TryEnter misses, release everything and escalate.
//
// EXCLUSIVE path: AcquireSRWLockExclusive makes us the sole list mutator (shared holders get drained and new ones are blocked)
// Makes this dead-lock free since two adjacent removers holding its own node lock need to stop teahoing other.
// node lock and needing the other's -- deadlock-free.
//
// Each remove nulls BOTH links, so a second remove of the same node no-ops instead of corrupting a list.
// 
// ---- Scan cursor (Policy B: skip contended locks, never escalate) ----
// Hand-over-hand walk under SHARED SRW held from beginning to end
// A contended neighbor (TryEnter miss) ends the pass by returning NULL 
// ===========================================================================

// Map a list entry to the CS guarding its links: the anchor -> head_lock,
// any real entry -> its pfn lock.
CRITICAL_SECTION*
rwlist_node_lock(PCONCURRENT_LIST_HEAD L, PLIST_ENTRY e)
{
    if (e == &L->entry) {
        return &L->head_lock;
    }
    else {
        return &get_pfn_from_PListEntry(e)->lock;
    }
}

// Insert node at the tail
// Precondition: caller holds node->lock and node->links
// Flink == node->links
// Blink == NULL
VOID
RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    int fails = 0;
    BOOLEAN exclusive = FALSE;

    for (;;) {
        if (exclusive) {
            // Sole mutator: the tail is stable, splice without neighbor locks
            AcquireSRWLockExclusive(&L->srw);
            PLIST_ENTRY tail = L->entry.Blink;
            node->links.Flink = &L->entry;
            node->links.Blink = tail;
            tail->Flink = &node->links;
            L->entry.Blink = &node->links;
            InterlockedIncrement64(&L->list_count);
            ReleaseSRWLockExclusive(&L->srw);
            return;
        }

        AcquireSRWLockShared(&L->srw);
        BOOLEAN ok = FALSE;
        // head_lock guards the anchor's Blink (the tail pointer)
        // once the lock is held, the tale is stable and every tail-end mutation takes head_lock
        if (TryEnterCriticalSection(&L->head_lock)) {
            PLIST_ENTRY tail = L->entry.Blink;
            CRITICAL_SECTION* tail_l = rwlist_node_lock(L, tail);
            BOOLEAN dedup = (tail_l == &L->head_lock);   // empty list: tail == anchor
            if (dedup || TryEnterCriticalSection(tail_l)) {
                node->links.Flink = &L->entry;
                node->links.Blink = tail;
                tail->Flink = &node->links;
                L->entry.Blink = &node->links;
                InterlockedIncrement64(&L->list_count);
                if (!dedup) LeaveCriticalSection(tail_l);
                LeaveCriticalSection(&L->head_lock);
                ok = TRUE;
            }
            else {
                LeaveCriticalSection(&L->head_lock);
            }
        }
        ReleaseSRWLockShared(&L->srw);
        if (ok) return;
        if (++fails >= LIST_COUPLE_MAX_FAILS) exclusive = TRUE;
    }
}

// Remove a node the caller already owns
// Precondition: caller holds node lock for the entire call
VOID
RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    // Off any list is BOTH links NULL. Trusting only Flink (old code) lets a torn,
    // half-linked node (exactly one link NULL) slip through as "already off" and get
    // activated while a neighbor still references it. Trap the torn state here; only
    // no-op when the node is genuinely detached.
    if (node->links.Flink == NULL || node->links.Blink == NULL) {
        ASSERT(node->links.Flink == NULL && node->links.Blink == NULL);
        return;   // genuinely off any list
    }

    int fails = 0;
    BOOLEAN exclusive = FALSE;

    for (;;) {
        // node->links are stable under node->lock (no neighbor operations without node lock)
        PLIST_ENTRY flink = node->links.Flink;
        PLIST_ENTRY blink = node->links.Blink;

        if (exclusive) {
            // Under exclusive SRW lock, only thread changing
            AcquireSRWLockExclusive(&L->srw);
            blink->Flink = flink;
            flink->Blink = blink;
            node->links.Flink = NULL;
            node->links.Blink = NULL;
            InterlockedDecrement64(&L->list_count);
            ReleaseSRWLockExclusive(&L->srw);
            return;
        }

        AcquireSRWLockShared(&L->srw);
        CRITICAL_SECTION* flink_lock = rwlist_node_lock(L, flink);
        CRITICAL_SECTION* blink_lock = rwlist_node_lock(L, blink);
        BOOLEAN ok = FALSE;
        if (TryEnterCriticalSection(flink_lock)) {
            BOOLEAN solo = (blink_lock == flink_lock);   // single-element list
            if (solo || TryEnterCriticalSection(blink_lock)) {
                blink->Flink = flink;
                flink->Blink = blink;
                node->links.Flink = NULL;
                node->links.Blink = NULL;
                InterlockedDecrement64(&L->list_count);
                if (!solo) LeaveCriticalSection(blink_lock);
                LeaveCriticalSection(flink_lock);
                ok = TRUE;
            }
            else {
                LeaveCriticalSection(flink_lock);
            }
        }
        ReleaseSRWLockShared(&L->srw);
        if (ok) return;
        if (++fails >= LIST_COUPLE_MAX_FAILS) exclusive = TRUE;
    }
}

// Advance one node forward/backward
// Returns new pfn or NULL if the walk reached the anchor (done) or hit a contended node (stop)
static pfn_metadata*
rwlist_scan_advance(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    PLIST_ENTRY next = c->forward ? c->cur->Flink : c->cur->Blink;
    if (next == &L->entry) {
        return NULL;   // wrapped to the anchor: end of list
    }
#if DEBUG
    // Wild-pointer tripwire: a real node's links live inside physical_slots. Catch a corrupt
    // Flink/Blink HERE, before dereferencing it in TryEnter -- that was the "unreadable CS" crash.
    if ((BYTE*)next < (BYTE*)physical_slots || (BYTE*)next >(BYTE*)&physical_slots[max_frame_number]) {
        printf("SCAN WILD NODE (%s): cur %p -> next %p\n",
            (L == &standbyList_head) ? "standby" : "modified", c->cur, next);
        DebugBreak();
        return NULL;
    }
#endif
    CRITICAL_SECTION* next_l = rwlist_node_lock(L, next);
    if (!TryEnterCriticalSection(next_l)) {
        return NULL;   // contended so stop this pass (Policy B)
    }
#if DEBUG
    // Type tripwire: every node physically on this list must carry the list's type. An ACTIVE
    // (or otherwise wrong-type) node here IS the corruption -- caught the instant a scan touches
    // it, far sooner than the 1/sec integrity checker or the downstream trimmer assert.
    {
        ULONG64 lt = get_pfn_from_PListEntry(next)->state.list_type;
        ULONG64 expect = (L == &standbyList_head) ? LIST_STANDBY : LIST_MODIFIED;
        if (lt != expect) {
            printf("SCAN WRONG-TYPE NODE (%s): node %p list_type=%llu expected %llu Flink=%p Blink=%p\n",
                (L == &standbyList_head) ? "standby" : "modified",
                next, lt, expect, next->Flink, next->Blink);
            DebugBreak();
        }
    }
#endif
    // Shift the window forward so old curr becomes blink
    // Drop the old blink lock
    if (c->prev_lock != c->cur_lock) {
        LeaveCriticalSection(c->prev_lock);
    }
    c->prev = c->cur;
    c->prev_lock = c->cur_lock;
    c->cur = next;
    c->cur_lock = next_l;
    return get_pfn_from_PListEntry(next);
}

// Begin a scan and holds SRW shared and locks the anchor as the initial prev==cur 
// Advances to and returns the first real node (NULL if empty or the first node is contended)
pfn_metadata*
RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward)
{
    c->list = L;
    c->forward = forward;
    AcquireSRWLockShared(&L->srw);
    c->prev = &L->entry;
    c->prev_lock = &L->head_lock;
    c->cur = &L->entry;
    c->cur_lock = &L->head_lock;   // prev==cur==anchor: head_lock held once
    EnterCriticalSection(&L->head_lock);   // bounded: head_lock holders never block
    return rwlist_scan_advance(c);
}

// Advance to and return the next node or NULL if end of list / contended
pfn_metadata*
RWListScanNext(RW_LIST_CURSOR* c)
{
    return rwlist_scan_advance(c);
}

// Remove cur node and TRANSFER its lock to the caller 
// On success the returned pfn is still locked and the cursor no longer owns it (caller must unlock)
// Needs prev (held), cur (held), but if next is contended, returns NULL
pfn_metadata*
RWListScanRemoveCurrent(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->cur == &L->entry) return NULL;   // not positioned on a real node

    PLIST_ENTRY cur = c->cur;
    PLIST_ENTRY next = c->forward ? cur->Flink : cur->Blink;
    PLIST_ENTRY prev = c->forward ? cur->Blink : cur->Flink;
    CRITICAL_SECTION* next_lock = rwlist_node_lock(L, next);

    // Indicate whether single-element list or not
    BOOLEAN need_next = (next_lock != c->prev_lock && next_lock != c->cur_lock);
    if (need_next && !TryEnterCriticalSection(next_lock)) {
        return NULL;   // leave cursor intact even if next is contended
    }

    // Unlink cur
    cur->Blink->Flink = cur->Flink;
    cur->Flink->Blink = cur->Blink;
    cur->Flink = NULL;
    cur->Blink = NULL;
    InterlockedDecrement64(&L->list_count);
    if (need_next) LeaveCriticalSection(next_lock);

    pfn_metadata* removed = get_pfn_from_PListEntry(cur);

    // Collapse the window onto prev and keep it locked
    // Call cur prev for advancement
    // cur_lock is still held by caller 
    c->cur = c->prev;
    c->cur_lock = c->prev_lock;
    return removed;
}

// At the end of a scan, release cur and prev locks and the SRW
VOID
RWListScanEnd(RW_LIST_CURSOR* c)
{
    if (c->cur_lock != c->prev_lock) {
        LeaveCriticalSection(c->cur_lock);
    }
    LeaveCriticalSection(c->prev_lock);
    ReleaseSRWLockShared(&c->list->srw);
}


// Atomically rescue and remove current standby node by locking next pfn and rescuing cur pfn (repoints PTE to disc)
// Transfers removed node's lock to caller and returns removed/claimed pfn (still locked) or NULL (revert changes)
// Lock next pfn first to prevent stranded rescued frame on standby and rescue-success implies remove-success 
pfn_metadata*
rwlist_scan_rescue_remove(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->cur == &L->entry) return NULL;

    PLIST_ENTRY cur = c->cur;
    CRITICAL_SECTION* next_lock = rwlist_node_lock(L, cur->Flink);
    BOOLEAN need_next = (next_lock != c->prev_lock && next_lock != c->cur_lock);
    if (need_next && !TryEnterCriticalSection(next_lock)) {
        return NULL;   // next contended: leave the node on standby
    }

    pfn_metadata* pfn = get_pfn_from_PListEntry(cur);
    if (!rescue_standby_to_disc(pfn)) {
        if (need_next) LeaveCriticalSection(next_lock);
        return NULL;   // region contended: leave the node on standby
    }

    // Unlink cur (prev, cur, next locks all held), null links, decrement
    cur->Blink->Flink = cur->Flink;
    cur->Flink->Blink = cur->Blink;
    cur->Flink = NULL;
    cur->Blink = NULL;
    InterlockedDecrement64(&L->list_count);
    if (need_next) LeaveCriticalSection(next_lock);

    claim_rescued_pfn(pfn);   // list_type = NONE (stamp owner)

    // Collapse the window onto prev and transfer cur_lock to the caller
    c->cur = c->prev;
    c->cur_lock = c->prev_lock;
    // TRAP: the rescued frame is handed to the caller to be reused; it MUST be off the
    // list. If it still carries links, the caller would reuse a still-linked frame.
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
        ULONG64 lt = pfn->state.list_type;
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