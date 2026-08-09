#include "vm.h"
#include "pfn.h"

// Claim a frame just pulled off standby so no other path can hand it out.
// Caller holds pfn->lock (the scan transfers cur_lock == pfn->lock to us), so the
// plain list_type write below is serialized by the frame lock.
VOID
claim_rescued_pfn(pfn_metadata* pfn)
{
    pfn->list_type = LIST_NONE;   // off standby, in-transit
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

    // A frame on the zero list MUST be off every other list. If it still reads
    // MODIFIED/STANDBY here we caught a double list membership at the second
    // allocation -- trap now (links not yet nulled, so Flink still names the list).
#if DEBUG
    if (pfn->list_type != LIST_NONE) {
        printf("BUG: get_pfn_from_zero handed out pfn %p still on list_type=%llu Flink=%p Blink=%p (double membership)\n",
            pfn, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
        DebugBreak();
    }
#endif

// Off the zero list so now owned/in-transit and null its links. Take pfn->lock so this
// (the only otherwise-lock-free links write) can't tear against a concurrent list op if
// the frame is momentarily double-membered.
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

// Returns free page from freeList
pfn_metadata*
get_pfn_from_free(VOID)
{
    int home = free_shard_for_thread();
    FREE_PAGE_CACHE* cache = &free_caches[home];

    if (cache->count == 0) {
        for (int k = 0; k < NUM_FREE_SHARDS; k++) {
            int shard = (home + k) % NUM_FREE_SHARDS;
            if (refill_free_cache(cache, shard) > 0) break;
        }
    }

    if (cache->count == 0) {
        return NULL; // all shards empty so caller falls through to standby and trimming pages
    }

    pfn_metadata* pfn = cache->pages[--cache->count];
    cache->pages[cache->count] = NULL;
    InterlockedDecrement64(&cached_pages);

    // A frame on the free shard/cache MUST be off every other list. If it still
    // reads MODIFIED/STANDBY here we caught a double list membership at the second
    // allocation -- trap now (links not yet nulled, so Flink still names the list).
#if DEBUG
    if (pfn->list_type != LIST_NONE) {
        printf("BUG: get_pfn_from_free handed out pfn %p still on list_type=%llu Flink=%p Blink=%p (double membership)\n",
            pfn, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
        DebugBreak();
    }
#endif

// Off the free shard/cache, now owned/in-transit: restore the not-on-any-list sentinel
// so a later RWListInsertTail (once active and trimmed) sees NULL links. Take pfn->lock so
// this (the only otherwise-lock-free links write) can't tear against a concurrent list op.
    EnterCriticalSection(&pfn->lock);
    pfn->links.Flink = NULL;
    pfn->links.Blink = NULL;
    LeaveCriticalSection(&pfn->lock);

    // DEBUG: catch double-allocation at the source
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev_owner = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev_owner != 0) {
        printf("BUG: get_pfn_from_free handed out pfn %p already owned by tid %llu (I am tid %llu)\n", pfn, prev_owner, tid);
        DebugBreak();
    }
    return pfn;
}

// Rescue a free frame from standby list in its disc state
pfn_metadata*
get_pfn_from_standby(VOID)
{
    RW_LIST_CURSOR cur;

    // Walk standby forward under shared SRW (Policy B)
    // On an eligible node, rescue and remove atomically then unlock its transferred lock
    for (pfn_metadata* pfn = RWListScanBegin(&standbyList_head, &cur, TRUE);
        pfn != NULL;
        pfn = RWListScanNext(&cur)) {
        if (pfn->list_type == LIST_STANDBY && !pfn->being_written) {
            pfn_metadata* got = rwlist_scan_rescue_remove(&cur);
            if (got != NULL) {
                RWListScanEnd(&cur);   // releases prev + SRW (we still own got_lock)
                unlock_pfn(got);
                return got;
            }
            // next / region contended so skip this node and keep scanning
        }
    }
    RWListScanEnd(&cur);
    return NULL;
}


// Getting a free pfn from free or standby list
pfn_metadata*
get_free_pfn(VOID)
{
    pfn_metadata* new_pfn;
    new_pfn = get_pfn_from_zero();
    if (new_pfn != NULL) return new_pfn;

    new_pfn = get_pfn_from_free();
    if (new_pfn != NULL) return new_pfn;

    new_pfn = get_pfn_from_standby();
    if (new_pfn != NULL) return new_pfn;

    return NULL; // caller will signal trimmer and wait
}