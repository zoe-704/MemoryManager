#include "vm.h"
#include "pfn.h"

// Claim a frame just pulled off standby under pfn lock
VOID
claim_rescued_pfn(pfn_metadata* pfn)
{
    pfn->list_type = LIST_NONE;   // off standby and in-transit
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev != 0) {
        printf("BUG: standby rescue handed out pfn %p already owned by tid %llu (I am tid %llu)\n",
            pfn, prev, tid);
        DebugBreak();
    }
}

// Getting PFN from entry on a PFN list
pfn_metadata*
get_pfn_from_PListEntry(PLIST_ENTRY entry)
{
    if (entry == NULL) return NULL;
    return CONTAINING_RECORD(entry, pfn_metadata, links);
}

// Getting PFN from a frame number
pfn_metadata*
get_pfn_from_fn(ULONG64 fn)
{
    if (fn > max_frame_number) {
        printf("FATAL: get_pfn_from_fn(%llu) out of range, max=%llu\n", fn, max_frame_number);
        DebugBreak();
        return NULL;
    }
    ASSERT(fn <= max_frame_number);
    return &physical_slots[fn];
}


// Returns free page from zeroList
pfn_metadata*
get_pfn_from_zero(VOID)
{
    EnterCriticalSection(&zeroList_head.list_lock);
    if (IsListEmpty(&zeroList_head.entry)) {
        LeaveCriticalSection(&zeroList_head.list_lock);
        return NULL;
    }
    // Get page off of zero list
    PLIST_ENTRY entry = RemoveHeadList(&zeroList_head.entry);
    InterlockedDecrement64(&zeroList_head.list_count);
    LeaveCriticalSection(&zeroList_head.list_lock);
    pfn_metadata* pfn = get_pfn_from_PListEntry(entry);

    // A frame on the zero list MUST be off every other list
    // Catch double list membership at the second allocation 
#if DEBUG
    if (pfn->list_type != LIST_NONE) {
        printf("BUG: get_pfn_from_zero handed out pfn %p still on list_type=%llu Flink=%p Blink=%p (double membership)\n",
            pfn, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
        DebugBreak();
    }
#endif

// Off the zero list so now owned/in-transit and null its links
    EnterCriticalSection(&pfn->lock);
    pfn->links.Flink = NULL;
    pfn->links.Blink = NULL;
    LeaveCriticalSection(&pfn->lock);

    // DEBUG: catch double-allocation at the source
#if DEBUG
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev_owner = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev_owner != 0) {
        printf("BUG: get_pfn_from_free handed out pfn %p already owned by tid %llu (I am tid %llu)\n", pfn, prev_owner, tid);
        DebugBreak();
    }
#endif
    return pfn;
}

// HOT PATH: pop a frame off the calling thread's magazine
// Returns NULL if the magazine is empty
static pfn_metadata*
magazine_pop(FREE_PAGE_CACHE* cache)
{
    if (cache->count == 0) return NULL;
    pfn_metadata* pfn = cache->pages[--cache->count];
    cache->pages[cache->count] = NULL;
    InterlockedDecrement64(&cached_pages);

#if DEBUG
    // Catch double-alloc/double-membership
    if (pfn->list_type != LIST_NONE ||
        pfn->links.Flink != NULL || pfn->links.Blink != NULL) {
        printf("BUG: magazine handed out pfn %p list_type=%llu Flink=%p Blink=%p (double membership)\n",
            pfn, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
        DebugBreak();
    }
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev != 0) {
        printf("BUG: magazine handed out pfn %p already owned by tid %llu (I am tid %llu)\n",
            pfn, prev, tid);
        DebugBreak();
    }
#endif
    return pfn;
}

// COLD PATH: magazine is empty so refill it from the shards in a batch or from standby
static pfn_metadata*
get_pfn_cold(void)
{
    int home = free_shard_for_thread();
    FREE_PAGE_CACHE* cache = &free_caches[home];

    for (int k = 0; k < NUM_FREE_SHARDS; k++) {
        int shard = (home + k) % NUM_FREE_SHARDS;
        if (refill_free_cache(cache, shard) > 0) break;
    }
    pfn_metadata* pfn = magazine_pop(cache);
    if (pfn != NULL) return pfn;

    // Steal a frame off standby
    return get_pfn_from_standby();
}

// Rescue a free frame from standby list in its disc state
pfn_metadata*
get_pfn_from_standby(VOID)
{
    RW_LIST_CURSOR cur;

    // Walk standby forward so rescue and remove a node atomically then unlock
    for (pfn_metadata* pfn = RWListScanBegin(&standbyList_head, &cur, TRUE);
        pfn != NULL;
        pfn = RWListScanNext(&cur)) {
        if (pfn->list_type == LIST_STANDBY && !pfn->being_written) {
            pfn_metadata* got = rwlist_scan_rescue_remove(&cur);
            if (got != NULL) {
                RWListScanEnd(&cur);   // releases prev and SRW
                unlock_pfn(got);
                return got;
            }
            // Next/region contended so skip this node and keep scanning
        }
    }
    RWListScanEnd(&cur);
    return NULL;
}


// Acquire a frame for a fault with a hot/cold split
// from_disc: fault overwrites whole frame from disc so does not need zero-ed page
// otherwise: use pre-zeroed frame and skip inline memset
// Returns NULL only when free shards and standby are exhausted so the caller wakes up other threads
pfn_metadata*
get_pfn_for_fault(BOOL from_disc)
{
    if (!from_disc) {
        pfn_metadata* z = get_pfn_from_zero();
        if (z != NULL) {
            // Demand-driven refill the reserve as it drains
            if ((ULONG64)zeroList_head.list_count < LOW_ZERO_PAGE_THRESHOLD) {
                SetEvent(startZero_event);
            }
            return z;
        }
    }

    // HOT PATH: lock-free magazine pop of a garbage frame, whic is fine for disc-backed
    pfn_metadata* pfn = magazine_pop(&free_caches[free_shard_for_thread()]);
    if (pfn != NULL) return pfn;

    // COLD PATH
    return get_pfn_cold();
}