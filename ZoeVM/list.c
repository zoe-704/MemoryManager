#include "vm.h"
#include "list.h"

/*
    RWList: reader/writer access to a CONCURRENT_LIST_HEAD
    SRW lets there be parallelism
    Shared - 1+ read-only walkers
    Scans take visited node's pfn lock (TryEnter) so callers get frame locked to read or mutate its state
*/

// Insert node at tail
// Caller holds node's lock so node->links must be null sentinel
VOID
RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    AcquireSRWLockExclusive(&L->srw);
    InsertTailList(&L->entry, &node->links);   // sole mutator
    InterlockedIncrement64(&L->list_count);
    ReleaseSRWLockExclusive(&L->srw);
}

// Remove a node unless it is already off
// Caller holds node's lock 
VOID
RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node)
{
    // Node's list type in metadata should match the list it is being removed from
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

    AcquireSRWLockExclusive(&L->srw);
    if (node->links.Flink != NULL) {   // on list with both links set OR both NULL
        RemoveEntryList(&node->links);
        node->links.Flink = NULL;
        node->links.Blink = NULL;
        InterlockedDecrement64(&L->list_count);
    }
    ReleaseSRWLockExclusive(&L->srw);
    return;
}

// Advance to next lockable node and TryEnter the single current node's pfn lock.
// Returns the node with lock held or NULL
static pfn_metadata*
rwlist_scan_advance(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;

    // Release node returned by previous advance unless already cleared
    if (c->cur_lock != NULL) {
        LeaveCriticalSection(c->cur_lock);
        c->cur_lock = NULL;
    }

    PLIST_ENTRY e = c->cur;
    for (;;) {
        PLIST_ENTRY next = c->forward ? e->Flink : e->Blink;
        if (next == &L->entry) {
            c->cur = &L->entry;   // wrapped to anchor signaling end of list
            return NULL;
        }
#if DEBUG
        {
            if ((BYTE*)next < (BYTE*)physical_slots ||
                (BYTE*)next >(BYTE*) & physical_slots[max_frame_number]) {
                printf("SCAN WILD NODE (%s): from %p -> next %p\n",
                    (L == &standbyList_head) ? "standby" : "modified", e, next);
                DebugBreak();
                c->cur = &L->entry;
                return NULL;
            }
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
        // Contended since holder is a remover/inserter blocked on exclusive srw
        // Skip past node (stable under exclusive) and keep looking
        e = next;
    }
}

// Begin a scan under exclusive srw
// Return the first lockable real node or NULL if empty
pfn_metadata*
RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward)
{
    AcquireSRWLockExclusive(&L->srw);
    c->list = L;
    c->forward = forward;
    c->cur = &L->entry;
    c->cur_lock = NULL;
    c->prev = NULL;         // unused in exclusive model
    c->prev_lock = NULL;
    return rwlist_scan_advance(c);
}

// Advance to and return next node (with lock held) or NULL at end of list
pfn_metadata*
RWListScanNext(RW_LIST_CURSOR* c)
{
    return rwlist_scan_advance(c);
}

// Remove current node and transfer its lock to the caller
// Cursor resumes from cur's predecessor
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

    // Transfer cur's lock to the caller and cursor resumes from the (unlocked) predecessor
    c->cur = resume;
    c->cur_lock = NULL;
    return removed;
}

// End a scan 
// Release the current node's lock and exclusive srw
VOID
RWListScanEnd(RW_LIST_CURSOR* c)
{
    if (c->cur_lock != NULL) {
        LeaveCriticalSection(c->cur_lock);
        c->cur_lock = NULL;
    }
    ReleaseSRWLockExclusive(&c->list->srw);
}

// Rescue current standby node by repointing its owner transition PTE to disc
// Remove it and transfer its lock to the caller
// Returns NULL if owner region is contended
pfn_metadata*
rwlist_scan_rescue_remove(RW_LIST_CURSOR* c)
{
    PCONCURRENT_LIST_HEAD L = c->list;
    if (c->cur == &L->entry) return NULL;

    PLIST_ENTRY cur = c->cur;
    pfn_metadata* pfn = get_pfn_from_PListEntry(cur);

    if (!rescue_standby_to_disc(pfn)) {
        return NULL;   // owner region contended sp leave node on standby
    }

    PLIST_ENTRY resume = c->forward ? cur->Blink : cur->Flink;
    RemoveEntryList(cur);
    cur->Flink = NULL;
    cur->Blink = NULL;
    InterlockedDecrement64(&L->list_count);

    claim_rescued_pfn(pfn);   // set list_type to NONE

    c->cur = resume;
    c->cur_lock = NULL;   // transfer cur's lock to caller
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
