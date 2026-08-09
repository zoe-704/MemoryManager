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

// HOT PATH: pop a frame off the calling thread's magazine. No lock -- the magazine is
// thread-local, and its frames already carry the NULL sentinel (links are nulled at refill
// time, off the hot path). Returns NULL if the magazine is empty. This is the common
// disc-backed-fault case and must stay lean.
static pfn_metadata*
magazine_pop(FREE_PAGE_CACHE* cache)
{
    if (cache->count == 0) return NULL;
    pfn_metadata* pfn = cache->pages[--cache->count];
    cache->pages[cache->count] = NULL;
    InterlockedDecrement64(&cached_pages);

#if DEBUG
    // Double-alloc / double-membership tripwire (compiled out of the release hot path).
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

// COLD PATH: the magazine is empty. Refill it from the shards (batched -- one shard lock per
// FREE_REFILL_BATCH pops) and pop; if every shard is dry, steal a frame directly off standby.
// Takes locks, but runs only once per batch, so its cost amortizes over the hot pops.
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

    // Shards dry: steal a frame off standby (repoints its owner PTE to disc).
    return get_pfn_from_standby();
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


// Acquire a frame for a fault (hot/cold split).
//   from_disc == TRUE  : the fault will overwrite the whole frame from disc, so it needs no
//                        zeros -> never touch the zero list; go straight to the hot magazine.
//   from_disc == FALSE : demand-zero first touch -> spend a pre-zeroed frame if we have one
//                        (skips the inline memset). Rare in steady state, so the zero list's
//                        global lock stays off the common (disc-backed) path.
// Returns NULL only when free shards AND standby are exhausted; the caller then wakes the
// producers and waits.
pfn_metadata*
get_pfn_for_fault(BOOL from_disc)
{
    if (!from_disc) {
        pfn_metadata* z = get_pfn_from_zero();
        if (z != NULL) {
            // Demand-driven top-up: refill the reserve as it drains, instead of the 1s poll.
            if ((ULONG64)zeroList_head.list_count < LOW_ZERO_PAGE_THRESHOLD) {
                SetEvent(startZero_event);
            }
            return z;
        }
    }

    // HOT PATH: lock-free magazine pop (garbage frame; fine for disc-backed and for demand-zero
    // once memset inline by the caller when the zero reserve was empty).
    pfn_metadata* pfn = magazine_pop(&free_caches[free_shard_for_thread()]);
    if (pfn != NULL) return pfn;

    // COLD PATH.
    return get_pfn_cold();
}