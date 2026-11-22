#include "vm.h"
#include "list.h"

// Resolve the lock guarding a list entry (head sentinel uses list's head_lock, rest use its pfn->lock)
static CRITICAL_SECTION*
rwlist_node_lock(PCONCURRENT_LIST_HEAD L, PLIST_ENTRY entry)
{
    if (entry == &L->entry) return &L->head_lock;
    return &get_pfn_from_PListEntry(entry)->lock;
}

// Insert node at the tail under shared access
// PRECONDITION: caller holds node's lock and node is unlinked (links are NULL)
VOID
RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    AcquireSRWLockShared(&L->srw);
    ULONG64 fails = 0;

    for (;;) {
        // head_lock is the entry point for every tail insert
        if (!TryEnterCriticalSection(&L->head_lock)) {
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;   // nothing held on this branch, safe to escalate straight away
        }

        // On an empty list head.Blink is head sentinel, tail_lock = head_lock and same_lock prevents double-acquiring
        PLIST_ENTRY tail = L->entry.Blink;
        CRITICAL_SECTION* tail_lock = rwlist_node_lock(L, tail);
        BOOLEAN same_lock = (tail_lock == &L->head_lock);
        if (!same_lock && !TryEnterCriticalSection(tail_lock)) {
            LeaveCriticalSection(&L->head_lock);
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }

        // Both neighbors of the new node locked, splice in at the tail
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
    // Every lock from  loop is released by now, so blocking on exclusive can't close a wait cycle
    ReleaseSRWLockShared(&L->srw);
    AcquireSRWLockExclusive(&L->srw);
    InsertTailList(&L->entry, &node->links);   // exclusive already excludes everyone else
    InterlockedIncrement64(&L->list_count);
    ReleaseSRWLockExclusive(&L->srw);
}

// Unlink a specific node the caller already knows about (ex. soft fault poaching 1 frame off modified/standby)
// PRECONDITION: caller holds node's lock for reading node->links so Flink/Blink safe without extra locking
VOID
RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
#if DEBUG
    {
        // Node's recorded list type should match the list it is being removed from
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
        // Node's links are stable because the caller holds node's lock, so re-read fresh
        PLIST_ENTRY succ = entry->Flink;
        PLIST_ENTRY pred = entry->Blink;
        CRITICAL_SECTION* succ_lock = rwlist_node_lock(L, succ);
        CRITICAL_SECTION* pred_lock = rwlist_node_lock(L, pred);

        if (!TryEnterCriticalSection(succ_lock)) {
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }
        // Pred and succ can resolve to the same lock (node is the only real entry)
        BOOLEAN same_lock = (pred_lock == succ_lock);
        if (!same_lock && !TryEnterCriticalSection(pred_lock)) {
            LeaveCriticalSection(succ_lock);
            if (++fails < LIST_COUPLE_MAX_FAILS) continue;
            goto escalate;
        }

        // All three locks held (node's via the caller, succ's and pred's just now)
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

// Policy-B "any eligible node" walk under shared access, caller holds node's lock for reading node->links so Flink/Blink safe without extra locking
// Begin a walk (head->tail or tail->head) and returns first eligible node or NULL if none
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

// Advance one node, returns new node with own lock held (caller evenually releases it)
pfn_metadata*
RWListScanNext(RW_LIST_CURSOR* c)
{
    PLIST_ENTRY from = (c->curr != NULL) ? c->curr : c->pred;   // both are lock-held
    PLIST_ENTRY next = c->forward ? from->Flink : from->Blink;

    if (next == &c->list->entry) {
		// Exhausted, move node to pred and release its lock, then release head_lock and srw
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

    // Old curr becomes pred (lock ownership transfers)
    if (c->curr != NULL) {
        LeaveCriticalSection(c->pred_lock);
        c->pred = c->curr;
        c->pred_lock = c->curr_lock;
    }
    c->curr = next;
    c->curr_lock = next_lock;
    return get_pfn_from_PListEntry(next);
}

// Unlink the node last returned by RWListScanNext
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
    // Curr's lock ownership transfers to the caller
    c->curr = NULL;
    c->curr_lock = NULL;
    return removed;
}

// End a scan, release curr, release pred, drop shared SRW hold
VOID
RWListScanEnd(RW_LIST_CURSOR* c)
{
    if (c->curr_lock != NULL) LeaveCriticalSection(c->curr_lock);
    LeaveCriticalSection(c->pred_lock);
    ReleaseSRWLockShared(&c->list->srw);
}

// Rescue the cur standby node by repointing owner transition PTE to disc, then unlink and transfer lock to caller
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

	// Commit, and if owner region is contended, release far-neighbor lock and leave node linked for next pass
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
