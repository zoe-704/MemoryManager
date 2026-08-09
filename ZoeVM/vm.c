// DEBUGGER CHEATSHEET

// Left 8 digit number = start address, right = end address

// ? = equation/quick math
// lm = list module (lists all modules and where they are)
// ln = list near (give any address, and it will give which variable it belongs to)
// r = registers (shows all the cpu registers and the values within those registers and their variables/locations/instruction)
// kn = stack trace (current stack pointer (at top of VA space), return address (location of var it's returning to), and the function it is in (called call site))
// dv = dump variables (will show locations and values of all variables local to the function)
// bp = breakpoint (set breakpoint at function. ex: bp vmTest!main)
// g = go until breakpoint, completion, or crash
// u = unasemble (tells all the future instructions in your function)
// bl = lists all your breakpoints
// dd = Tries to show you the contents at an address
// .f+ go to next frame (who was the function before me?)
// dq = dump quad (dumps the values of 8 byte chunks at address specified)
// .logopen (opens text file with all the output of the debugger)
// .logclose
// gh = go ahead (degugger don't worry just continue)
// sxd av = (av = access violation) (tells debugger to stop breaking on this particular exception)*********\
// !vprot = (tells you if its legit and gives you the address for the memory allocation and the state ex. MEM_RESERVE, etc.)
// q = quit process
// bd = remove breakpoint (ex. bd 1 removes breakpoint 1)
// ?? var_name = gives value of that variable
// x = see list of globals
// ~*k dumps all threads running
// x ZoeVM!*page_count
// !critsec pte_lock

// Performance trace cheat sheet
// xperf -on base -stackwalk profile
// Then run your program
// xperf -stop -d trace1.etl
// wpa trace1.etl
// Once in the trace, click Trace and then Load Symbols

// GLOBAL LOCK ORDER:
// region (pte) lock
//      ↓
// list lock(modified / standby / free)
//      ↓
// pfn lock(per - page)
//      ↓
// disc lock
// Back - edges(region acquired while holding a list) MUST use TryEnter and skip on failure

#include "vm.h"

// Single definitions for the perf-stat globals declared extern in vm.h
MM_STAT g_trim_stat;
MM_STAT g_write_stat;
LARGE_INTEGER g_qpc_freq;   // set once via QueryPerformanceFrequency

// Functions for high-quality random number generation
VOID 
SeedRng(THREAD_RNG_STATE* rng) 
{
    ULONG64 seed = __rdtsc() ^ ((ULONG64)GetCurrentThreadId() * 0x9E3779B97F4A7C15ULL);
    // splitmix64 to expand one seed into two well-mixed state words
    ULONG64 z = (seed += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    rng->s0 = z ^ (z >> 31);

    z = (seed += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    rng->s1 = z ^ (z >> 31);

    if (rng->s0 == 0 && rng->s1 == 0) rng->s0 = 1; // avoid all-zero state
    rng->counter = 0;
}

ULONG64 
GetNextRandom(THREAD_RNG_STATE* rng) 
{
    ULONG64 s1 = rng->s0;
    const ULONG64 s0 = rng->s1;
    const ULONG64 result = s0 + s1;

    rng->s0 = s0;
    s1 ^= s1 << 23;
    rng->s1 = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);

    rng->counter++;
    if ((rng->counter & 0xFFFF) == 0) 
    {
        rng->s0 ^= __rdtsc();
        if (rng->s0 == 0 && rng->s1 == 0) rng->s0 = 1;
    }

    return result;
}


// Tracking statistics

// Read the raw performance counter as a signed tick count
// Paired with g_qpc_freq to convert deltas to time and callers subtract two
// QPC() samples and hand the delta to stat_add
static __forceinline LONG64
QPC(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Fold one timed operation into a shared MM_STAT accumulator
// Use interlocked adds bc trim/write stats are written by several threads without locks
//   t     = primary duration in raw QPC ticks (e.g. map time)
//   tsub  = secondary duration in ticks (e.g. memcpy time), 0 if unused
//   pages = frames processed this call, for pages/call averages
static __forceinline VOID
stat_add(MM_STAT* s, LONG64 t, LONG64 tsub, LONG64 pages) {
    InterlockedIncrement64(&s->calls);
    InterlockedAdd64(&s->ticks, t);
    InterlockedAdd64(&s->ticks_sub, tsub);
    InterlockedAdd64(&s->pages, pages);
}

// Convert raw QPC ticks to milliseconds using the process-wide frequency
// g_qpc_freq must be set once (QueryPerformanceFrequency) before any stats priint since guard returns 0.0
static DOUBLE
ms(LONG64 ticks) {
    // Guard against an unset g_qpc_freq: a future regression prints 0.0, not inf.
    return g_qpc_freq.QuadPart ? (double)ticks * 1000.0 / (double)g_qpc_freq.QuadPart : 0.0;
}

// Allocate and manage each thread's allocated scratch pages in temp_va_base
__declspec(thread) ULONG stage_cursor = 0;

// Each thread owns a non-overlapping THREAD_SCRATCH_PAGES pages chunk of shared temp_va_base to avoid collisions
// Asserts thread_index must be set b/c  the __declspec(thread) default of -1 would compute negative offset
static __forceinline PULONG_PTR
thread_scratch_base(VOID)
{
    ASSERT(thread_index >= 0 && thread_index < NUM_THREADS);
    return (PULONG_PTR)((char*)temp_va_base + (SIZE_T)thread_index * THREAD_SCRATCH_PAGES * PAGE_SIZE);
}

// Single-page sub-region used to zero a frame in isolation (scrub_frame),
// mapped and unmapped without disturbing the stage or write regions.
static __forceinline PULONG_PTR
thread_scrub_slot(VOID) 
{ 
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)SLOT_SCRUB * PAGE_SIZE); 
}

// Base of this thread's WRITE_BATCH-page window, used by write_modified_list
// (and zero_thread) to batch-map a run of frames for a single map/copy/unmap
static __forceinline PULONG_PTR
thread_write_base(VOID) 
{ 
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)OFF_WRITE * PAGE_SIZE); 
}

// Base of this thread's staging ring, used on the hard-fault disc-restore path
// to map frames one slot at a time and amortize the unmap over many faults
static __forceinline PULONG_PTR
thread_stage_base(VOID)
{
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)OFF_STAGE * PAGE_SIZE);
}

// Unmap every slot filled since the last flush, in one syscall (collapses many per-fault unmaps into single batched call
// stage_cursor is thread-local, so this only touches this thread's ring and doesn't need a lock
VOID
stage_ring_flush(VOID)
{
    if (stage_cursor == 0) return;
    if (MapUserPhysicalPages(thread_stage_base(), stage_cursor, NULL) == FALSE) {
        printf("stage_ring_flush: batch unmap failed, count=%lu, err %lu\n",
            stage_cursor, GetLastError());
        DebugBreak();
    }
    stage_cursor = 0;
}

// Claim the next slot, mapping the caller's frame into it
PULONG_PTR
stage_ring_map(ULONG64 frame_number)
{
    if (stage_cursor >= STAGE_RING_PAGES) {
        stage_ring_flush();
    }
    PULONG_PTR slot = (PULONG_PTR)((char*)thread_stage_base() + (SIZE_T)stage_cursor * PAGE_SIZE);
    ULONG_PTR fn = frame_number;
    if (MapUserPhysicalPages(slot, 1, &fn) == FALSE) {
        printf("stage_ring_map: map frame %llX failed, err %lu\n",
            frame_number, GetLastError());
        DebugBreak();
        return NULL;
    }
    stage_cursor++;
    return slot;
}




// ---- Region active-list + region-age index ----

// Highest age with a non-empty active bucket (the region's oldest resident page), or
// REGION_AGE_NONE if the region has no active pages. Caller holds region->lock.
ULONG64
region_oldest_age(PPTE_REGION region)
{
    for (int a = AGES - 1; a >= 0; a--) {
        if (region->age_counts[a] != 0) return (ULONG64)a;
    }
    return REGION_AGE_NONE;
}

// Move the region to the region-age list matching its current oldest age (no-op if already
// there). Caller holds region->lock (age_counts are stable); we take region_age_lock to touch
// the age lists and the region's link fields. Lock order is always region->lock then
// region_age_lock, never the reverse.
VOID
region_rebucket(PPTE_REGION region)
{
    ULONG64 want = region_oldest_age(region);
    if (want == region->age_list_number) return;

    EnterCriticalSection(&region_age_lock);
    if (region->age_list_number != REGION_AGE_NONE) {
        RemoveEntryList(&region->age_link);
        region_age_lists[region->age_list_number].count--;
    }
    if (want != REGION_AGE_NONE) {
        InsertTailList(&region_age_lists[want].head, &region->age_link);
        region_age_lists[want].count++;
    }
    region->age_list_number = want;
    LeaveCriticalSection(&region_age_lock);
}

// Put a just-activated frame onto its region's age bucket (the coherent home of an ACTIVE
// frame). Caller holds region->lock; pfn is exclusively owned or pfn->lock held. pfn->links
// must be the NULL sentinel (frame just came off free/zero/standby). Re-buckets the region.
VOID
region_add_active(PPTE_REGION region, pfn_metadata* pfn, ULONG64 age)
{
    // The frame must be the NULL sentinel here (off every list). A non-NULL link means it is
    // still on another list -- adding it now would double-member it and later crash a scan.
#if DEBUG
    if (pfn->links.Flink != NULL || pfn->links.Blink != NULL) {
        printf("ADD_ACTIVE DOUBLE-MEMBER: pfn=%p frame=%llu list_type=%llu Flink=%p Blink=%p\n",
            pfn, pfn->frame_number, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
        DebugBreak();
    }
#endif
    InsertTailList(&region->active_age_lists[age], &pfn->links);
    region->age_counts[age]++;
    region->active_page_count++;
    region_rebucket(region);
}

// Trim one region: CLAIM frames off its oldest active buckets (oldest first) and push them
// onto the modified list. Caller holds region->lock. Coherent by construction: RemoveEntryList
// off the active bucket is the ownership handoff -- once a frame is off the active list, no
// other path can reach it (active frames are found ONLY through this list, never by PTE), so
// there is no double-use to guard against. Unmap the whole batch BEFORE publishing so the disc
// writer never sees a still-mapped frame.
static VOID
trim_region(PPTE_REGION region, ULONG64 oldest, int* batch_count, INT batch_size, ULONG* trimmed)
{
    PULONG_PTR    unmap_vas[MAX_TRIM_PAGES];
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES];
    ULONG n = 0;

    // Phase 1: claim victims off the oldest buckets, flip each PTE to transition.
    for (int a = (int)oldest; a >= 0 && *batch_count < batch_size && n < MAX_TRIM_PAGES; a--) {
        while (!IsListEmpty(&region->active_age_lists[a]) &&
               *batch_count < batch_size && n < MAX_TRIM_PAGES) {
            PLIST_ENTRY e = RemoveHeadList(&region->active_age_lists[a]);
            pfn_metadata* pfn = get_pfn_from_PListEntry(e);
            // A node on the active list MUST be a real ACTIVE frame with a PTE. If not, the
            // active list is corrupt -- trap here, before we deref pfn->pte and crash.
#if DEBUG
            if ((BYTE*)pfn < (BYTE*)physical_slots ||
                (BYTE*)pfn >(BYTE*)&physical_slots[max_frame_number] ||
                pfn->list_type != LIST_ACTIVE || pfn->pte == NULL) {
                printf("TRIM ACTIVE-LIST CORRUPT: region=%p bucket=%d pfn=%p list_type=%llu pte=%p "
                       "Flink=%p Blink=%p\n",
                    region, a, pfn, (ULONG64)pfn->list_type, pfn->pte,
                    pfn->links.Flink, pfn->links.Blink);
                DebugBreak();
            }
#endif
            pfn->links.Flink = NULL;   // off the active list, in-transit: restore the sentinel
            pfn->links.Blink = NULL;
            region->age_counts[a]--;
            if (region->active_page_count > 0) region->active_page_count--;

            EnterCriticalSection(&pfn->lock);
            PPTE pte = pfn->pte;   // stable: active frame, region lock serializes its PTE
            PULONG_PTR victim_va = get_va_from_pte(pte);
            set_pte_transition(pte, pfn->frame_number);
            pfn->list_type = LIST_NONE;   // off active, not yet on modified
            LeaveCriticalSection(&pfn->lock);

            unmap_vas[n] = (PVOID)((ULONG_PTR)victim_va & ~(PAGE_SIZE - 1));
            unmap_pfns[n] = pfn;
            n++;
            (*batch_count)++;
        }
    }

    if (n == 0) return;

    // Phase 2: batch scatter-unmap BEFORE publishing (victim VAs are scattered, so the scatter
    // variant is required -- MapUserPhysicalPages would leave all but the first mapped).
    if (MapUserPhysicalPagesScatter(unmap_vas, (ULONG_PTR)n, NULL) == FALSE) {
        printf("trim: batch unmap failed, n=%lu\n", n);
        DebugBreak();
    }

    // Phase 3: publish onto modified under each frame lock (list_type authoritative).
    for (ULONG k = 0; k < n; k++) {
        pfn_metadata* pfn = unmap_pfns[k];
        EnterCriticalSection(&pfn->lock);
        pfn->list_type = LIST_MODIFIED;
        RWListInsertTail(&modifiedList_head, pfn);
        LeaveCriticalSection(&pfn->lock);
        (*trimmed)++;
    }

    // Oldest bucket(s) drained: the region's oldest age may have dropped, or it may be empty.
    region_rebucket(region);
}

// Getting page candidates to trim: drain the OLDEST region-age lists first. Each region is
// claimed off the region-age index (region_age_lock), then its oldest active frames are
// harvested under the region lock. No PTE scan, no age_counts fast-skip guessing.
VOID
get_unmap_candidates_and_trim(int* batch_count, INT batch_size)
{
#if STATISTICS
    LONG64 t0 = QPC();
#endif
    ULONG trimmed = 0;

    for (int age = AGES - 1; age >= 0 && *batch_count < batch_size; age--) {
        // Bound this pass to the current membership so it can't spin on a rotating list.
        EnterCriticalSection(&region_age_lock);
        ULONG64 iterations = region_age_lists[age].count;
        LeaveCriticalSection(&region_age_lock);

        while (iterations-- > 0 && *batch_count < batch_size) {
            // Rotate the head region to the tail (forward progress, no immediate re-grab).
            // Only region_age_lock here; we take the region lock AFTER releasing it.
            EnterCriticalSection(&region_age_lock);
            if (IsListEmpty(&region_age_lists[age].head)) {
                LeaveCriticalSection(&region_age_lock);
                break;
            }
            PLIST_ENTRY e = RemoveHeadList(&region_age_lists[age].head);
            InsertTailList(&region_age_lists[age].head, e);
            PPTE_REGION region = CONTAINING_RECORD(e, PTE_REGION, age_link);
            LeaveCriticalSection(&region_age_lock);

            if (!TryEnterCriticalSection(&region->lock)) continue;
            // Re-check it is still on this age list (it may have been re-bucketed since the peek).
            if (region->age_list_number == (ULONG64)age && region->active_page_count > 0) {
                trim_region(region, (ULONG64)age, batch_count, batch_size, &trimmed);
            }
            LeaveCriticalSection(&region->lock);
        }
    }

#if STATISTICS
    stat_add(&g_trim_stat, QPC() - t0, 0, trimmed);
#else
    InterlockedAdd64(&g_trim_stat.pages, trimmed);   // pages feed the rate control, keep them live
#endif
    return;
}

// Shard for calling thread (fault threads index 0...7 map 1:1)
int
free_shard_for_thread(VOID)
{
    int t = thread_index;
    if (t < 0) t = 0;
    return t % NUM_FREE_SHARDS;
}

// Total pages currently sitting on the free shards (no cached pages)
static ULONG64
free_pages_total(VOID)
{
    ULONG64 sum = 0;
    for (int s = 0; s < NUM_FREE_SHARDS; s++)
        sum += freeList_shards[s].list_count;
    return sum;
}

// Total active (valid) pages across all PTE regions == total pages on the age lists
static ULONG64
active_pages_total(VOID)
{
    ULONG64 sum = 0;
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++)
        sum += pte_regions[i].active_page_count;
    return sum;
}


// Move up to FREE_REFILL_BATCH pages from one shard into cache
ULONG
refill_free_cache(FREE_PAGE_CACHE* cache, int shard) {
    PLIST_HEAD sh = &freeList_shards[shard];
    if (!TryEnterCriticalSection(&sh->list_lock)) return 0;
    ULONG moved = 0;
    while (cache->count < FREE_CACHE_MAX && moved < FREE_REFILL_BATCH && !IsListEmpty(&sh->entry)) {
        PLIST_ENTRY e = RemoveHeadList(&sh->entry);
        InterlockedDecrement64(&sh->list_count);
        cache->pages[cache->count++] = get_pfn_from_PListEntry(e);
        moved++;
    }
    LeaveCriticalSection(&sh->list_lock);
    if (moved) InterlockedAdd64(&cached_pages, (LONG64)moved);
    return moved;
}


// Scans the whole page so every non-zero 8-byte slot equals its own VA
// After a frame is mapped and populated
VOID
validate_page_contents(PVOID page_va, const char* site)
{
    PULONG_PTR base = (PULONG_PTR)((ULONG_PTR)page_va & ~(PAGE_SIZE - 1));
    for (ULONG64 i = 0; i < PAGE_SIZE / sizeof(ULONG_PTR); i++) {
        ULONG_PTR expected = (ULONG_PTR)(base + i);
        if (base[i] != 0 && base[i] != expected) {
            printf("BAD PAGE at [%s]: slot %p holds %p (expected 0 or %p) tid %lu\n",
                site, (PVOID)(base + i), (PVOID)base[i], (PVOID)expected, GetCurrentThreadId());
            DebugBreak();
            return;
        }
    }
}

// Create fake disc
PVOID
create_page_file(PULONG64 number_of_pages)
{
    PVOID p;
    if (*number_of_pages > MAX_DISC_SIZE) {
        *number_of_pages = MAX_DISC_SIZE;
    }
    ULONG64 num_bytes = *number_of_pages * PAGE_SIZE;
    p = malloc(num_bytes); // does not need to be memset bc will be overwritten 
    while (p == NULL) {
        num_bytes /= 2;
        p = malloc(num_bytes);
    }
    *number_of_pages = num_bytes / PAGE_SIZE;
    disc_metadata = malloc(*number_of_pages * sizeof(DISC_METADATA));
    if (disc_metadata == NULL) {
        printf("create_page_file: could not allocate disc_metadata and fake disc\n");
        DebugBreak();
    }
    memset(disc_metadata, 0, *number_of_pages * sizeof(DISC_METADATA));
    return p;
}

// Returns a region with free space and returns it locked (or NULL if disc full)
// Caller must LeaveCriticalSection(&reg->lock) when done
static PDISC_REGION
acquire_freest_disc_region(VOID)
{
    for (;;) {
        // Step 1: find the region with the highest free_count (unlocked read)
        PDISC_REGION best = NULL;
        ULONG64 best_free = 0;
        for (ULONG64 r = 0; r < DISC_REGIONS; r++) {
            ULONG64 f = disc_regions[r].free_count;   // racy read
            if (f > best_free) { best_free = f; best = &disc_regions[r]; }
        }
        if (best == NULL) return NULL;   // every region read as full

        // Step 2: lock the candidate and confirm it still has room
        EnterCriticalSection(&best->lock);
        if (best->free_count > 0) return best;   // caller unlocks
        LeaveCriticalSection(&best->lock);
        // Lost the race and someone filled it so retry scan
    }
}

// Return first free disc slot to fill
ULONG64
get_disk_free_slots(VOID)
{
    InterlockedIncrement64(&disk_debug[0]);

    PDISC_REGION disc_reg = acquire_freest_disc_region();
    if (disc_reg == NULL) return(ULONG64) - 1;              // disc is full

    // Find free byte starting at cursor and wrap once
    ULONG64 n = disc_reg->slot_count;
    ULONG64 c = disc_reg->cursor;
    ULONG64 local = (ULONG64)-1;
    for (LONG64 i = 0; i < n; i++) {
        LONG64 idx = c + i;
        if (idx >= n) idx -= n;
        if (disc_reg->bytemap[idx] == 0) {
            local = idx;
            break;
        }
    }

    // There were free pages so there must be a free slot
    ASSERT(local != (ULONG64)-1);
    disc_reg->bytemap[local] = 1;
    disc_reg->free_count--;
    disc_reg->cursor = (local + 1 < n) ? local + 1 : 0;

    ULONG64 global_slot = disc_reg->base_slot + local;
    LeaveCriticalSection(&disc_reg->lock);

    InterlockedIncrement64(&disk_debug[1]);
    ASSERT(global_slot < disc_page_count);
    return global_slot;
}

// Push back to stack and mark metadata as free
VOID
return_disk_free_slots(
    ULONG64 slot
) {
    ASSERT(slot < disc_page_count);

    // Map global slot->region and guard last possibly larger region
    ULONG64 disc_idx = slot / DISC_SLOTS_PER_REGION;
    if (disc_idx >= DISC_REGIONS) {
        disc_idx = DISC_REGIONS - 1;
    }
    PDISC_REGION disc_reg = &disc_regions[disc_idx];
    ULONG64 local = slot - disc_reg->base_slot;
    ASSERT(local < disc_reg->slot_count);

    EnterCriticalSection(&disc_reg->lock);
    ASSERT(disc_reg->bytemap[local] == 1);  // must be occupied
    //BOOL disc_was_full = (all_regions_full()); // ZS?
    disc_reg->bytemap[local] = 0;
    disc_reg->free_count++;
    // Bias cursor back so just-freed slot fond quickly
    disc_reg->cursor = local;
    LeaveCriticalSection(&disc_reg->lock);
}

// Count how many free disc slots
static ULONG64
disc_free_pages(VOID)
{
    ULONG64 free = 0;
    for (ULONG64 r = 0; r < DISC_REGIONS; r++)
        free += (ULONG64)disc_regions[r].free_count;
    return free > disc_page_count ? disc_page_count : free;   // clamp against read-skew
}
// Age pages
static __forceinline double 
clampd(double v, double lo, double hi) 
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Age a slice of regions: for every ACTIVE frame, read/clear its PTE access bit and MOVE the
// frame between its region's age buckets (accessed -> bucket 0, untouched -> bucket age+1).
// The buckets ARE the age accounting now, so aging is a list splice, not a histogram tweak.
// Runs under region->lock only (list_type stays ACTIVE, so no per-frame lock is needed).
VOID
AgeSweep(ULONG64 regions_to_sweep)
{
    InterlockedIncrement64(&tick_call);
    for (ULONG64 n = 0; n < regions_to_sweep; n++) {
        ULONG64 i = age_cursor;
        age_cursor = (age_cursor + 1) % NUM_PTE_LOCKS;

        PPTE_REGION region = &pte_regions[i];
        EnterCriticalSection(&region->lock);
        if (region->active_page_count == 0) {
            LeaveCriticalSection(&region->lock);
            continue;
        }

        // Decide each frame's new bucket by its PTE access bit, but DON'T move it mid-walk:
        // moving a frame to a not-yet-visited bucket would re-age it this same sweep. Record
        // the moves while walking, then apply them all afterward. A region spans PTES_PER_LOCK
        // PTEs, so it can hold at most that many active frames -> the scratch is bounded.
        pfn_metadata* moved[PTES_PER_LOCK];
        BYTE from_age[PTES_PER_LOCK];
        BYTE to_age[PTES_PER_LOCK];
        ULONG m = 0;

        for (int a = 0; a < AGES && m < PTES_PER_LOCK; a++) {
            PLIST_ENTRY head = &region->active_age_lists[a];
            for (PLIST_ENTRY cur = head->Flink; cur != head && m < PTES_PER_LOCK; cur = cur->Flink) {
                pfn_metadata* pfn = get_pfn_from_PListEntry(cur);
                // Every node on an active bucket must be a real ACTIVE frame; a wrong-typed or
                // wild node here is the corruption, caught before we touch its PTE.
#if DEBUG
                if ((BYTE*)pfn < (BYTE*)physical_slots ||
                    (BYTE*)pfn >(BYTE*)&physical_slots[max_frame_number] ||
                    pfn->list_type != LIST_ACTIVE) {
                    printf("AGE ACTIVE-LIST CORRUPT: region=%p bucket=%d pfn=%p list_type=%llu "
                           "Flink=%p Blink=%p\n",
                        region, a, pfn, (ULONG64)pfn->list_type, pfn->links.Flink, pfn->links.Blink);
                    DebugBreak();
                }
#endif
                PPTE pte = pfn->pte;   // stable: active frame under our region lock
                if (pte == NULL) continue;

                ULONG64 old = *(volatile ULONG64*)pte;
                PTE snap; snap.entire_contents = old;
                if (snap.hardware.valid != 1) continue;

                ULONG64 new_age;
                if (snap.hardware.accessed)   new_age = 0;              // touched -> hottest
                else if (a < AGES - 1)        new_age = (ULONG64)a + 1; // untouched -> colder
                else                          continue;                 // cold & maxed: leave it

                // Clear the access bit and stamp the new age atomically (races the access-bit
                // setters on the fault path). On collision, skip and pick it up next tick.
                PTE upd = snap;
                upd.hardware.accessed = 0;
                upd.hardware.age = new_age;
                if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
                        (LONG64)upd.entire_contents, (LONG64)old) != old) {
                    continue;
                }

                if (new_age != (ULONG64)a) {
                    moved[m] = pfn; from_age[m] = (BYTE)a; to_age[m] = (BYTE)new_age; m++;
                }
            }
        }

        // Apply the recorded bucket moves (splice + count bookkeeping).
        for (ULONG k = 0; k < m; k++) {
            RemoveEntryList(&moved[k]->links);
            region->age_counts[from_age[k]]--;
            InsertTailList(&region->active_age_lists[to_age[k]], &moved[k]->links);
            region->age_counts[to_age[k]]++;
        }

        // The region's oldest age may have changed -> move it on the region-age index.
        region_rebucket(region);
        LeaveCriticalSection(&region->lock);
    }
}

VOID
print_age_list_histogram (VOID)
{
    ULONG64 buckets[AGES] = { 0 };
    ULONG64 total_active = 0;
    ULONG64 sum_active = 0;
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        EnterCriticalSection(&pte_regions[i].lock);
        for (int age = 0; age <= 7; age++) {
            buckets[age] += pte_regions[i].age_counts[age];
        }
        sum_active += pte_regions[i].active_page_count;
        LeaveCriticalSection(&pte_regions[i].lock);
    }

    printf("\n---------- AGE LIST HISTOGRAM ----------\n");
    for (int age = 0; age <= 7; age++) {
        printf("  age %d: %6llu pages\n", age, buckets[age]);
        total_active += buckets[age];
    }
    printf("  ------------------------------\n");
    printf("  total in age lists : %6llu\n", total_active);
    printf("  free list          : %6llu\n", free_pages_total());
    printf("  active pages       : %6llu\n", sum_active);
    printf("  modified list      : %6llu\n", modifiedList_head.list_count);
    printf("  standby list       : %6llu\n", standbyList_head.list_count);
    printf("  disc used          : %6lld / %6llu\n", disc_page_count - disc_free_pages(), disc_page_count);
    printf("  cached             : %6llu\n", cached_pages);
    printf("  tick call          : %6llu\n", tick_call);
    printf("  sum active_page_count: %6llu  (should equal total in age lists)\n", sum_active);
    printf("  list totals: %6llu  (should equal %u)\n",
        free_pages_total() + cached_pages + zeroList_head.list_count +
        sum_active + modifiedList_head.list_count +
        standbyList_head.list_count,
        NUMBER_OF_PHYSICAL_PAGES);
    printf("-----------------------------------------\n\n");
}

VOID
print_statistics(VOID)
{
    ULONG64 total = NUMBER_OF_PHYSICAL_PAGES;
    ULONG64 free_c = free_pages_total() + cached_pages;
    ULONG64 act_c = active_pages_total();
    ULONG64 mod_c = InterlockedOr64(&modifiedList_head.list_count, 0);
    ULONG64 stby_c = InterlockedOr64(&standbyList_head.list_count, 0);
    ULONG64 zero_c = InterlockedOr64(&zeroList_head.list_count, 0);
    ULONG64 hard = InterlockedOr64(&hard_fault_count, 0);
    ULONG64 soft = InterlockedOr64(&soft_fault_count, 0);
    ULONG64 faults = hard + soft;

    printf("\n---------- STATISTICS ----------\n");
    printf("  FREE:     %6llu  (%.1f%%)\n", free_c, 100.0 * free_c / total);
    printf("  ACTIVE:   %6llu  (%.1f%%)\n", act_c,  100.0 * act_c / total);
    printf("  MODIFIED: %6llu  (%.1f%%)\n", mod_c,  100.0 * mod_c / total);
    printf("  STANDBY:  %6llu  (%.1f%%)\n", stby_c, 100.0 * stby_c / total);
    printf("  ZERO:     %6llu  (%.1f%%)\n", zero_c, 100.0 * zero_c / total);
    if (faults > 0) {
        printf("  HARD:     %6llu  (%.1f%%)\n", hard, 100.0 * hard / faults);
        printf("  SOFT:     %6llu  (%.1f%%)\n", soft, 100.0 * soft / faults);
    }
    printf("--------------------------------\n\n");
}

VOID print_mm_stats(void) {
    LONG64 tc = g_trim_stat.calls ? g_trim_stat.calls : 1;
    LONG64 wc = g_write_stat.calls ? g_write_stat.calls : 1;

    printf("Trim:  calls=%lld  sec=%.3f  ms/call=%.4f  pages/call=%.2f\n",
        g_trim_stat.calls,
        ms(g_trim_stat.ticks) / 1000.0,
        ms(g_trim_stat.ticks) / tc,
        (double)g_trim_stat.pages / tc);

    printf("Write: calls=%lld  map_ms=%.4f  memcpy_ms=%.4f  pages/call=%.2f\n",
        g_write_stat.calls,
        ms(g_write_stat.ticks) / wc,
        ms(g_write_stat.ticks_sub) / wc,
        (double)g_write_stat.pages / wc);
}

// Move modified list to disk and push to standby.
// Returns the number of frames drained this pass; 0 means the modified list had nothing
// eligible right now (empty, or every candidate contended / already being written). Callers
// loop on this progress signal rather than on modifiedList_head.list_count, whose exact
// value is only a best-effort hint under the concurrent-list lock-coupling.
ULONG
write_modified_list(VOID)
{
    PULONG_PTR batch_base = thread_write_base();
    ULONG_PTR frames[WRITE_BATCH];
    pfn_metadata* pfns[WRITE_BATCH];
    ULONG64 slots[WRITE_BATCH];
    ULONG count = 0;

    // Step 1: collect a batch off the modified list. Walk forward under shared SRW
    // (Policy B). For an eligible frame, RESERVE it (grab a disc slot + set being_written)
    // BEFORE removing, then RWListScanRemoveCurrent. The slot + being_written are cleanly
    // reversible, so if the removal loses the successor race we undo them and skip, leaving
    // the frame on modified -- unlike the standby rescue, no atomic-with-rescue op is needed.
    RW_LIST_CURSOR cur;
    for (pfn_metadata* pfn = RWListScanBegin(&modifiedList_head, &cur, TRUE);
         pfn != NULL && count < WRITE_BATCH;
         pfn = RWListScanNext(&cur)) {
        if (pfn->list_type == LIST_MODIFIED && !pfn->being_written) {
            ULONG64 slot = get_disk_free_slots();
            if (slot == (ULONG64)-1) {
                // disk full: leave this frame on modified and stop collecting
                SetEvent(redoFault_event);
                break;
            }
            // Scan holds cur_lock == pfn->lock, so these plain state writes are serialized.
            pfn->being_written = 1;
            pfn_metadata* removed = RWListScanRemoveCurrent(&cur);
            if (removed == NULL) {
                // successor contended: undo the reservation, leave frame on modified
                pfn->being_written = 0;
                return_disk_free_slots(slot);
                continue;
            }
            pfns[count] = removed;
            frames[count] = removed->frame_number;
            slots[count] = slot;
            count++;
            unlock_pfn(removed);   // transferred lock; frame is off modified with being_written=1
        }
    }
    RWListScanEnd(&cur);

    // Step 2: batch map, c amount of copies, batch unmap
    if (count > 0) {
#if STATISTICS
        LONG64 t0 = QPC();
#endif
        // Batch map
        if (MapUserPhysicalPages(batch_base, count, frames) == FALSE) {
            printf("batch map failed, count=%lu\n", count);
            DebugBreak();
        }
#if STATISTICS
        LONG64 t_map = QPC() - t0;
        LONG64 t_copy_total = 0;
#endif

        // Create c amount of copies
        for (ULONG i = 0; i < count; i++) {
            if (pfns[i]->being_written == 0) {
                // Rescued during collection so mark slot for return after unmap
                slots[i] = (ULONG64)-1;
                continue;
            }

            // Memcpy data to disk
            PULONG_PTR src = (PULONG_PTR)((char*)batch_base + (i * PAGE_SIZE));
#if STATISTICS
            LONG64 t1 = QPC();
#endif
            memcpy((BYTE*)disc + slots[i] * PAGE_SIZE, src, PAGE_SIZE);
#if STATISTICS
            t_copy_total += QPC() - t1;
#endif
        }
        // Batch unmap
        if (MapUserPhysicalPages(batch_base, count, NULL) == FALSE) {
            printf("batch map failed, count=%lu\n", count);
            DebugBreak();
        }
#if STATISTICS
        stat_add(&g_write_stat, t_map, t_copy_total, count);
#else
        InterlockedAdd64(&g_write_stat.pages, count);   // pages feed the rate control, keep them live
#endif
        
        // Step 3: commit to standby, GROUPED BY PTE REGION so each region lock is taken once
        // per batch (like the reference writer's Phase 3) instead of once per frame. Under the
        // region lock -- the serializer with the soft/hard-fault PTE path -- re-validate the
        // frame is still ours (not poached back to active) before inserting to standby. If
        // poached, hand the disc slot back and DON'T put it on standby. This is what keeps an
        // active frame off the standby list. Mirrors `poached || c->pte != evict_pte`.
        PPTE              evict_pte_of[WRITE_BATCH];
        CRITICAL_SECTION* region_of[WRITE_BATCH];
        BOOLEAN           committed[WRITE_BATCH];
        ULONG64           slot_to_free[WRITE_BATCH];

        // Pass 1: snapshot each frame's PTE (under its lock) and resolve its region lock.
        for (ULONG i = 0; i < count; i++) {
            slot_to_free[i] = (ULONG64)-1;
            committed[i] = (slots[i] == (ULONG64)-1);   // rescued in step 2, already handled
            evict_pte_of[i] = NULL;
            region_of[i] = NULL;
            if (committed[i]) continue;
            EnterCriticalSection(&pfns[i]->lock);
            evict_pte_of[i] = pfns[i]->pte;
            LeaveCriticalSection(&pfns[i]->lock);
            region_of[i] = (evict_pte_of[i] != NULL) ? get_pte_lock(evict_pte_of[i]) : NULL;
        }

        // Pass 2: for each not-yet-handled frame, take its region lock ONCE and process every
        // remaining frame that maps to the same region lock under that single hold.
        for (ULONG i = 0; i < count; i++) {
            if (committed[i]) continue;
            CRITICAL_SECTION* region = region_of[i];
            if (region) EnterCriticalSection(region);

            for (ULONG j = i; j < count; j++) {
                if (committed[j] || region_of[j] != region) continue;
                committed[j] = TRUE;

                pfn_metadata* c = pfns[j];
                PPTE evict_pte = evict_pte_of[j];
                EnterCriticalSection(&c->lock);

                BOOLEAN still_ours = FALSE;
                if (evict_pte != NULL && c->pte == evict_pte && c->being_written != 0) {
                    PTE snap;
                    snap.entire_contents = *(volatile ULONG64*)evict_pte;
                    still_ours = (snap.hardware.valid == 0) &&
                                 (snap.transition.transition == 1) &&
                                 (snap.transition.frame_number == c->frame_number);
                }

                if (!still_ours) {
#if DEBUG
                    // A being_written frame is off every list and soft faults bail on it, so it
                    // CANNOT be legitimately re-activated -- every poach here is the root
                    // corruption. Trap the first one live and dump why it failed.
                    PTE dbg; dbg.entire_contents = (evict_pte != NULL) ? *(volatile ULONG64*)evict_pte : 0;
                    printf("POACH: pfn=%p frame=%llu evict_pte=%p cur_pte=%p bw=%llu list_type=%llu | "
                           "valid=%llu trans=%llu disc=%llu ptefn=%llu\n",
                        c, c->frame_number, evict_pte, c->pte,
                        (ULONG64)c->being_written, (ULONG64)c->list_type,
                        (ULONG64)dbg.hardware.valid, (ULONG64)dbg.transition.transition,
                        (ULONG64)dbg.disc.disc, (ULONG64)dbg.transition.frame_number);
                    DebugBreak();
#endif
                    c->being_written = 0;
                    LeaveCriticalSection(&c->lock);
                    slot_to_free[j] = slots[j];   // hand back after the region lock is dropped
                    continue;
                }

                c->disc_index = slots[j];
                c->being_written = 0;
                c->list_type = LIST_STANDBY;
                ASSERT(c->links.Flink == NULL && c->links.Blink == NULL);
                RWListInsertTail(&standbyList_head, c);
                LeaveCriticalSection(&c->lock);
            }

            if (region) LeaveCriticalSection(region);
        }

        // Pass 3: free the poached slots after every region lock is dropped (region -> disc,
        // never nested).
        for (ULONG i = 0; i < count; i++) {
            if (slot_to_free[i] != (ULONG64)-1) {
                return_disk_free_slots(slot_to_free[i]);
                InterlockedIncrement64(&disk_debug[5]);
            }
        }
        // Signal fault threads that standby has pages
        SetEvent(redoFault_event);
    }
    return count;
}

ULONG64
zero_pfns(PULONG_PTR batch_base)
{

    ULONG_PTR frames[WRITE_BATCH];
    pfn_metadata* pfns[WRITE_BATCH];
    ULONG count = 0;

    // Step 1: claim up to WRITE_BATCH frames off of free shards
    for (int s = 0; s < NUM_FREE_SHARDS && count < WRITE_BATCH; s++) {
        PLIST_HEAD sh = &freeList_shards[s];
        EnterCriticalSection(&sh->list_lock);
        while (count < WRITE_BATCH && !IsListEmpty(&sh->entry)) {
            PLIST_ENTRY free_e = RemoveHeadList(&sh->entry);
            InterlockedDecrement64(&sh->list_count);
            pfn_metadata* pfn = get_pfn_from_PListEntry(free_e);
            pfns[count] = pfn;
            frames[count] = pfn->frame_number;
            count++;
        }
        LeaveCriticalSection(&sh->list_lock);
    }

    // Pages [0, from_free) came off the free shards as garbage -> they get zeroed onto the zero
    // list. Pages [from_free, count) are rescued off standby -> they go back onto the free shards
    // as garbage (and get zeroed onto the zero list on a later pass).
    ULONG from_free = count;

    // Step 2: claim frames off the standby list via the atomic rescue-to-disc scan.
    // Each rescued node's lock is transferred to us; unlock it once collected (it is
    // claimed and off every list, so nothing else can reach it).
    {
        RW_LIST_CURSOR cur;
        for (pfn_metadata* pfn = RWListScanBegin(&standbyList_head, &cur, TRUE);
             pfn != NULL && count < WRITE_BATCH;
             pfn = RWListScanNext(&cur)) {
            if (pfn->list_type == LIST_STANDBY && !pfn->being_written) {
                pfn_metadata* got = rwlist_scan_rescue_remove(&cur);
                if (got != NULL) {
                    pfns[count] = got;
                    frames[count] = got->frame_number;
                    count++;
                    unlock_pfn(got);   // transferred lock; done touching it here
                }
                // successor / region contended: skip, keep scanning
            }
        }
        RWListScanEnd(&cur);
    }
    if (count == 0) return 0;

    // Step 3: zero only the free-shard pages (map, memset, unmap); standby-rescued pages stay garbage
    if (from_free > 0) {
        if (MapUserPhysicalPages(batch_base, from_free, frames) == FALSE) {
            printf("zero_thread: batch map failed, count=%lu\n", from_free);
            DebugBreak();
        }
        for (ULONG i = 0; i < from_free; i++) {
            memset((char*)batch_base + (SIZE_T)i * PAGE_SIZE, 0, PAGE_SIZE);
        }
        if (MapUserPhysicalPages(batch_base, from_free, NULL) == FALSE) {
            printf("zero_thread: batch unmap failed, count=%lu\n", from_free);
            DebugBreak();
        }
    }

    // Step 4a: publish zeroed pages -> zero list (clean)
    if (from_free > 0) {
        EnterCriticalSection(&zeroList_head.list_lock);
        for (ULONG i = 0; i < from_free; i++) {
            pfns[i]->is_zero = 1;
            pfns[i]->pte = NULL;
            pfns[i]->disc_index = INVALID_DISC_SLOT;
            pfns[i]->list_type = LIST_NONE;             // exclusively owned here, off every list
            pfns[i]->owner_thread_id = 0;               // release so consumer re-claims
            InsertTailList(&zeroList_head.entry, &pfns[i]->links);
            InterlockedIncrement64(&zeroList_head.list_count);
        }
        LeaveCriticalSection(&zeroList_head.list_lock);
    }

    // Step 4b: publish standby-rescued pages -> free shards (garbage)
    for (int s = 0; s < NUM_FREE_SHARDS; s++) {
        PLIST_HEAD sh = &freeList_shards[s];
        EnterCriticalSection(&sh->list_lock);
        for (ULONG i = from_free + s; i < count; i += NUM_FREE_SHARDS) {
            pfns[i]->is_zero = 0;                       // free list holds garbage
            pfns[i]->pte = NULL;
            pfns[i]->disc_index = INVALID_DISC_SLOT;
            pfns[i]->list_type = LIST_NONE;             // exclusively owned here, off every list
            pfns[i]->owner_thread_id = 0;               // release so consumer re-claims
            InsertTailList(&sh->entry, &pfns[i]->links);
            InterlockedIncrement64(&sh->list_count);
        }
        LeaveCriticalSection(&sh->list_lock);
    }
    SetEvent(redoFault_event);   // pages available
    return count;
}

DWORD WINAPI
page_fault_thread_random (PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;
    // Each thread gets its own local rng state seeded from timestamp
    THREAD_RNG_STATE thread_rng;
    SeedRng(&thread_rng);
    ULONG64 random_number;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Page fault thread random %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    unsigned i = 0;
    BOOL page_faulted;
    BOOL fault_resolution = TRUE;
    PULONG_PTR arbitrary_va;
    ULONG64 runtime = (MB_MUL * (MB(1) / MB_DIV));

    // Now perform random accesses.
    while (i < runtime) {
        InterlockedIncrement64(&loop_iterations);
        page_faulted = FALSE;

        // If arbitrary VA is empty or successfully stamped, generate next arbitrary VA
        if (fault_resolution == TRUE) {
            random_number = GetNextRandom(&thread_rng);
            random_number %= virtual_address_size_in_unsigned_chunks;

            random_number &= ~0x7;
            arbitrary_va = VA_SPACE + random_number;
            fault_resolution = FALSE;
        }

        // Some VAs can map to the same page and will not fault
        __try {
            ULONG_PTR current_value = *(volatile PULONG_PTR)arbitrary_va;

            // If the page isn't blank (0), it MUST match our VA
            if (current_value != 0 && current_value != (ULONG_PTR)arbitrary_va) {
                printf("CRITICAL: Data corruption! VA %p was overwritten with %p\n", arbitrary_va, (PVOID)current_value);
                DebugBreak();
            }

            *(PULONG_PTR)arbitrary_va = (ULONG_PTR)arbitrary_va;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }

        PPTE pte = get_pte_from_va(arbitrary_va);
        // Handle page fault 
        if (page_faulted) {
            // If page faulted, we want to redo this iteration to confirm successful mapping
            i--;
            fault_resolution = FALSE;

            // Snapshot the PTE once and a concurrent writer can change it between reads
            PTE snap;
            *(ULONG64*)&snap = *(volatile ULONG64*)pte;

            // pte/region/page-aligned VA are fixed by the faulting VA -- derive once and thread
            // them into the handlers (and the hard->soft hand-off) instead of recomputing.
            PPTE_REGION region_struct = get_pte_region(pte);
            PVOID page_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

            // Check 1: another thread may have already resolved this fault
            if (snap.hardware.valid == 1) {
                continue;
            }

            // Check 2: another fault thread may have already rescued this
            else if (snap.transition.valid == 0 && snap.transition.transition == 1) {
                if (!handle_soft_fault(arbitrary_va, pte, region_struct, page_aligned)) {
                    fault_resolution = FALSE;
                }
            }

            // Check 3: we will hard fault if pte from disc or completely new
            else if (snap.hardware.valid == 0 && snap.transition.transition == 0) {
                if (!handle_hard_fault(arbitrary_va, pte, region_struct, page_aligned)) {
                    fault_resolution = FALSE;
                }
            }

            // Check 4: unexpected change to pte, try same va again
            else {
                continue;
            }
        }
        // Fault does not occur
        else {
            InterlockedIncrement64(&va_access_count);
            pte_set_accessed(pte);   // reuse the pte already derived above
            fault_resolution = TRUE;
        }
        if (i > 0 && i % 10000 == 0) {
            printf(".");
            fflush(stdout);
        }
        i++;
    }

    stage_ring_flush();

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Thread %i finished accessing %u virtual addresses in %.2f ms\n", thread_index, i, elapsed_ms);
    return 0;
}

DWORD WINAPI
page_fault_thread_nonrandom (PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;
    THREAD_RNG_STATE thread_rng;
    SeedRng(&thread_rng);

    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Page fault thread nonrandom %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    //
    // Locality state.
    //
    ULONG64 total_pages = virtual_address_size_in_unsigned_chunks / (PAGE_SIZE / sizeof(ULONG_PTR));

    // Bound the working set so trimmed pages are still on standby when revisited.
    ULONG64 cold_span = total_pages / WORKING_SET_DIVISOR;
    if (cold_span == 0) {
        cold_span = total_pages;
    }

    // Each thread gets its own slice so threads don't serialize on the same region locks.
    ULONG64 slice = cold_span / NUM_FAULT_THREADS;
    ULONG64 slice_lo = (ULONG64)thread_index * slice;
    if (slice == 0) {
        slice = cold_span;
        slice_lo = 0;
    }

    HOT_SPOT hot[HOT_SPOTS];
    for (int h = 0; h < HOT_SPOTS; h++) {
        hot[h].base = 0;
        hot[h].len = 0;
    }
    int hot_next = 0;
    int hot_count = 0;

    ULONG64 base_page = slice_lo + (GetNextRandom(&thread_rng) % slice);
    ULONG64 cur_page = base_page;
    ULONG64 run_left = 0;

    unsigned i = 0;
    BOOL page_faulted;
    BOOL fault_resolution = TRUE;
    PULONG_PTR arbitrary_va = NULL;
    ULONG64 runtime = (MB_MUL * (MB(1) / MB_DIV));

    while (i < runtime) {
        InterlockedIncrement64(&loop_iterations);
        page_faulted = FALSE;

        //
        // Only pick a new VA when the previous one was fully resolved.
        //
        if (fault_resolution == TRUE) {

            if (run_left == 0) {
                ULONG64 r = GetNextRandom(&thread_rng);

                if (hot_count > 0 && (r % REVISIT_CHANCE) == 0) {
                    //
                    // REVISIT: re-touch the same base AND length as before, so those
                    // pages are still valid or on modified/standby -- soft faults.
                    //
                    int pick;
                    if ((GetNextRandom(&thread_rng) % RECENT_BIAS) == 0) {
                        int recent = (hot_count < 4) ? hot_count : 4;
                        int back = 1 + (int)(GetNextRandom(&thread_rng) % recent);
                        pick = (hot_next - back + HOT_SPOTS) % HOT_SPOTS;
                    }
                    else {
                        pick = (int)(GetNextRandom(&thread_rng) % hot_count);
                    }
                    base_page = hot[pick].base;
                    run_left = hot[pick].len;
                }
                else {
                    // COLD JUMP with locality skew: ~90% of jumps land in a small hot zone
                    // (stays young), ~10% reach the wider slice. Those wide pages are touched
                    // rarely, sit idle, and age up -> a real 0..7 spread.
                    ULONG64 span = ((GetNextRandom(&thread_rng) % HOT_ZONE_BIAS) != 0)
                        ? (slice / HOT_ZONE_DIVISOR)   // common: small hot zone
                        : slice;                        // rare: whole slice
                    if (span == 0) span = 1;
                    base_page = slice_lo + (GetNextRandom(&thread_rng) % span);
                    run_left = MIN_RUN_PAGES +
                        (GetNextRandom(&thread_rng) % (MAX_RUN_PAGES - MIN_RUN_PAGES));
                }

                // Clamp to the end of the VA space.
                if (base_page >= total_pages) {
                    base_page = 0;
                }
                if (base_page + run_left > total_pages) {
                    run_left = total_pages - base_page;
                    if (run_left == 0) {
                        base_page = 0;
                        run_left = MIN_RUN_PAGES;
                    }
                }

                // Stamp as most recent so repeated revisits keep a small set hot.
                hot[hot_next].base = base_page;
                hot[hot_next].len = run_left;
                hot_next = (hot_next + 1) % HOT_SPOTS;
                if (hot_count < HOT_SPOTS) {
                    hot_count++;
                }

                cur_page = base_page;
            }

            arbitrary_va = (PULONG_PTR)((PUCHAR)VA_SPACE + (cur_page * PAGE_SIZE));
            fault_resolution = FALSE;
        }

        page_faulted = FALSE;

        __try {
            ULONG_PTR current_value = *(volatile PULONG_PTR)arbitrary_va;

            if (current_value != 0 && current_value != (ULONG_PTR)arbitrary_va) {
                printf("CRITICAL: Data corruption! VA %p was overwritten with %p\n",
                    arbitrary_va, (PVOID)current_value);
                DebugBreak();
            }

            *(PULONG_PTR)arbitrary_va = (ULONG_PTR)arbitrary_va;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }

        PPTE pte = get_pte_from_va(arbitrary_va);

        if (page_faulted) {
            // Redo this iteration against the SAME va but do not advance the run
            i--;
            fault_resolution = FALSE;

            PTE snap;
            *(ULONG64*)&snap = *(volatile ULONG64*)pte;

            // Derive pte/region/page-aligned VA once and thread them into the handlers.
            PPTE_REGION region_struct = get_pte_region(pte);
            PVOID page_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

            if (snap.hardware.valid == 1) {
                continue;
            }
            else if (snap.transition.valid == 0 && snap.transition.transition == 1) {
                if (!handle_soft_fault(arbitrary_va, pte, region_struct, page_aligned)) {
                    fault_resolution = FALSE;
                }
            }
            else if (snap.hardware.valid == 0 && snap.transition.transition == 0) {
                if (!handle_hard_fault(arbitrary_va, pte, region_struct, page_aligned)) {
                    fault_resolution = FALSE;
                }
            }
            else {
                continue;
            }
        }
        else {
            InterlockedIncrement64(&va_access_count);
            pte_set_accessed(pte);   // reuse the pte already derived above
            // Access succeeded so advance within the run
            cur_page++;
            run_left--;
            fault_resolution = TRUE;

        }
        if (i > 0 && i % MB(1) == 0) {
            printf(".");
            fflush(stdout);
        }
        i++;
    }
    stage_ring_flush();

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Thread %i finished accessing %u virtual addresses in %.2f ms\n", thread_index, i, elapsed_ms);
    return 0;
}

// thread type 2
DWORD WINAPI
trim_thread(PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;
    int count = 0;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Trim thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    for (;;) {
        LONG64 high_consumption = InterlockedOr64(&g_trim_full_throttle, 0);
        if (high_consumption) {
            // Trimming is falling behind so don't block on startTrim_event, keep on trimming
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
        }
        else {
            // Trimming is keeping up so sleep until someone asks for pages
            HANDLE waits[2] = { startTrim_event, shutdown_event };
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
        }

        // Backpressure: if the write pipeline is backed up, yield and let the writer drain
        // instead of dumping more pages onto the modified list.
        if (modifiedList_head.list_count > MODIFIED_HIGH_WATER) {
            SetEvent(modifiedReady_event);
            if (WaitForSingleObject(shutdown_event, 1) == WAIT_OBJECT_0) break;
            continue;
        }

        LONG64 batch = InterlockedOr64(&g_trim_target, 0);
        if (batch <= 0) batch = MIN_TRIM_BATCH;
        if (batch > MAX_TRIM_PAGES) batch = MAX_TRIM_PAGES;

        count = 0;
        get_unmap_candidates_and_trim(&count, (INT)batch);

        // Nothing left to trim right now so yield ZS schedule
        if (high_consumption && count == 0 && WaitForSingleObject(shutdown_event, 1) == WAIT_OBJECT_0) break;
    }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Trim thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

// thread type 3
DWORD WINAPI
disc_thread(PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Disc thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);
    
    for (;;) {
        // Sleep until trim thread signals there's work
        HANDLE waits[2] = { modifiedReady_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        // Drain the modified list by ACTUAL PROGRESS, not by list_count. Each pass returns
        // how many frames it committed; stop when a pass drains nothing (list empty or every
        // candidate momentarily contended -- Policy B, they get picked up on the next wake).
        // Looping on list_count instead would spin forever if the concurrent-list counter
        // ever over-reports, since that value is only an approximate hint here.
        for (;;) {
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
            if (write_modified_list() == 0) break;
        }
    }

    // Drain anything remaining before exit
    while (write_modified_list() > 0) { /* keep draining until empty */ }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Disc thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

// thread type 4: aging (pressure-driven)
DWORD WINAPI
age_thread(PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Age thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    for (;;) {
        HANDLE waits[2] = { startAge_event, shutdown_event };
        if (WaitForSingleObject(shutdown_event, AGE_TICK_MS) == WAIT_OBJECT_0) break;
        AgeSweep((ULONG64)InterlockedOr64(&g_age_regions_per_tick, 0));
    }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Age thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

// thread type 5: periodic
DWORD WINAPI
periodic_thread(PVOID parameter) 
{
    thread_index = (int)(ULONG_PTR)parameter;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Periodic thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);


    LONG64 prev_hard = 0, prev_soft = 0, prev_trimmed = 0, prev_written = 0;
    ULONG64 prev_tick = GetTickCount64();
    ULONG64 t_start = prev_tick;
    ULONG64 t0 = GetTickCount64();

    for (;;) {
        if (WaitForSingleObject(shutdown_event, 1000) == WAIT_OBJECT_0) break;

        ULONG64 now = GetTickCount64();
        ULONG64 dt_ms = now - prev_tick;
        if (dt_ms == 0) continue;
        prev_tick = now;

        LONG64 hard = InterlockedOr64(&hard_fault_count, 0);
        LONG64 soft = InterlockedOr64(&soft_fault_count, 0);
        LONG64 trimmed = InterlockedOr64(&g_trim_stat.pages, 0);
        LONG64 written = InterlockedOr64(&g_write_stat.pages, 0);

        double consume_rate = (double)(hard - prev_hard) * 1000.0 / dt_ms;
        double trim_rate = (double)(trimmed - prev_trimmed) * 1000.0 / dt_ms;
        double write_rate = (double)(written - prev_written) * 1000.0 / dt_ms;
        double soft_rate = (double)(soft - prev_soft) * 1000.0 / dt_ms;

        prev_hard = hard; 
        prev_soft = soft;
        prev_trimmed = trimmed; 
        prev_written = written;

        // Ready pages are immediately usable; in-flight (modified) pages are already trimmed
        // and just awaiting write-back, so they count toward the trim runway but not toward aging.
        ULONG64 ready = free_pages_total()
            + zeroList_head.list_count
            + standbyList_head.list_count;
        ULONG64 in_flight = modifiedList_head.list_count;
        ULONG64 supply = ready + in_flight;
        double remaining_s = (consume_rate > 0.0) ? (double)supply / consume_rate : 1e9;

        // Writer is behind while the modified backlog is over the high-water mark; trimming
        // pauses until it drains so we don't pile more onto an already-full modified list.
        LONG64 writer_behind = (in_flight > MODIFIED_HIGH_WATER);

        // Dynamic aging
        // steady-state/idle: set baseline target sweep time 
        // otherwise: calculate time to sweep over whole set at current rate
        double valid_count = (double)active_pages_total();  // active pages are the valid pages

        double T_sweep_ms;
        if (consume_rate < 1.0) {
            T_sweep_ms = AGE_TSWEEP_BASELINE_MS;   // idle: keep ages fresh & differentiated (e.g. 300 ms)
        }
        else {
            // V/C = time to churn the whole valid set at current pressure (natural eviction timescale)
            T_sweep_ms = (valid_count / (AGE_ALPHA * consume_rate)) * 1000.0;
            if (ready < LOW_FREE_PAGE_THRESHOLD) T_sweep_ms *= 0.5;  // pool draining: sharpen ages faster
            T_sweep_ms = clampd(T_sweep_ms, AGE_TSWEEP_MIN_MS, AGE_TSWEEP_BASELINE_MS);
        }

        LONG64 rpt = (LONG64)(NUM_PTE_LOCKS * (double)AGE_TICK_MS / T_sweep_ms);
        InterlockedExchange64(&g_age_regions_per_tick, rpt < 1 ? 1 : rpt);

        // Dynamic trimming
        // consume_rate > trim_rate falling behind so max trimming
        // otherwise: balance with consumption rate
        LONG64 behind = (consume_rate > trim_rate) && !writer_behind;
        InterlockedExchange64(&g_trim_full_throttle, behind);

        ULONG64 want;
        if (behind) {
            want = MAX_TRIM_PAGES;
        }
        else {
            want = (ULONG64)(consume_rate * TARGET_REMAINING_S);
            if (want < MIN_TRIM_BATCH) want = MIN_TRIM_BATCH;
            if (want > MAX_TRIM_PAGES) want = MAX_TRIM_PAGES;
        }
        InterlockedExchange64(&g_trim_target, (LONG64)want);

        SetEvent(startAge_event);
        if (!writer_behind && (behind || remaining_s < TARGET_REMAINING_S)) SetEvent(startTrim_event);
        if (modifiedList_head.list_count > 1000) SetEvent(modifiedReady_event);
        if (zeroList_head.list_count < LOW_ZERO_PAGE_THRESHOLD) SetEvent(startZero_event);

#if DEBUG
        // Integrity sweep once per tick: catches list corruption within ~1s of introduction,
        // with the list still intact, instead of minutes later as a wild pointer.
        check_concurrent_list(&standbyList_head, LIST_STANDBY, "standby");
        check_concurrent_list(&modifiedList_head, LIST_MODIFIED, "modified");
#endif

#if DEBUG
        printf("\n[%6llums] act=%6lld mod=%6lld stby=%6lld free=%5lld zero=%5lld disc=%6lld "
            "| consume=%7.0f trim=%7.0f write=%7.0f soft=%7.0f | remaining=%.2fs\n",
            now - t_start,
            active_pages_total(), modifiedList_head.list_count,
            standbyList_head.list_count, free_pages_total(),
            zeroList_head.list_count, disc_page_count - disc_free_pages(),
            consume_rate, trim_rate, write_rate, soft_rate,
            remaining_s);
#endif
    }


    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Periodic thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

// thread type 6: zero
DWORD WINAPI
zero_thread(PVOID parameter) 
{
    thread_index = (int)(ULONG_PTR)parameter;
    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Zero thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    // Zero thread's write region in the scratch slab (WRITE_BATCH pages) but not the 1-page scrub slot
    // Requires this thread_index to own a WRITE_BATCH-page write region in temp_va_base
    PULONG_PTR batch_base = thread_write_base();

    for (;;) {
        HANDLE waits[2] = { startZero_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        for (;;) {
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
            if (zero_pfns(batch_base) == 0) break;
        }
    }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Zero thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

VOID
full_virtual_memory_test(
    VOID
)
{
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    BOOL page_faulted;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    if (!setup_program()) {
        printf("full_virtual_memory_test: setup failed\n");
        return;
    }

    // Create two page fault threads
    HANDLE threads[NUM_THREADS] = { NULL };
    for (int i = 0; i < NUM_FAULT_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0, page_fault_thread_nonrandom, (PVOID)(ULONG_PTR)i, 0, NULL);
        //threads[i] = CreateThread(NULL, 0, page_fault_thread_random, (PVOID)(ULONG_PTR)i, 0, NULL);
    }
    // Worker thread indices MUST follow the fault threads contiguously (0..NUM_FAULT_THREADS-1
    // are the fault threads), so index them off NUM_FAULT_THREADS -- never hardcode. Hardcoding
    // left a gap at index NUM_FAULT_THREADS and pushed the last worker to NUM_THREADS, which
    // overruns free_caches[] / the per-thread scratch slab and trips the thread_scratch_base assert.
    threads[NUM_FAULT_THREADS]   = CreateThread(NULL, 0, trim_thread,     (PVOID)(ULONG_PTR)(NUM_FAULT_THREADS + 0), 0, NULL);
    threads[NUM_FAULT_THREADS+1] = CreateThread(NULL, 0, disc_thread,     (PVOID)(ULONG_PTR)(NUM_FAULT_THREADS + 1), 0, NULL);
    threads[NUM_FAULT_THREADS+2] = CreateThread(NULL, 0, age_thread,      (PVOID)(ULONG_PTR)(NUM_FAULT_THREADS + 2), 0, NULL);
    threads[NUM_FAULT_THREADS+3] = CreateThread(NULL, 0, periodic_thread, (PVOID)(ULONG_PTR)(NUM_FAULT_THREADS + 3), 0, NULL);
    threads[NUM_FAULT_THREADS+4] = CreateThread(NULL, 0, zero_thread,     (PVOID)(ULONG_PTR)(NUM_FAULT_THREADS + 4), 0, NULL);

    if (threads[0] == NULL || threads[1] == NULL || threads[2] == NULL) {
        printf("Failed to create threads. Error: %lu\n", GetLastError());
        return;
    }
    // Wait for fault threads to finish
    WaitForMultipleObjects(NUM_FAULT_THREADS, threads, TRUE, INFINITE);
    
    // Print statistics
    print_age_list_histogram();
    print_statistics();
    print_mm_stats();

    SetEvent(shutdown_event);
    WaitForMultipleObjects(NUM_THREADS - NUM_FAULT_THREADS, &threads[NUM_FAULT_THREADS], TRUE, INFINITE);

    // Close all handles
    for (int i = 0; i < NUM_THREADS; i++) {
        CloseHandle(threads[i]);
    }
    CloseHandle(startAge_event);
    CloseHandle(startTrim_event);
    CloseHandle(startZero_event);
    CloseHandle(diskReady_event);
    CloseHandle(modifiedReady_event);
    CloseHandle(redoFault_event);
    CloseHandle(shutdown_event);

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Program: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

    // Delete all locks
    for (int i = 0; i < NUM_FREE_SHARDS; i++) {
        DeleteCriticalSection(&freeList_shards[i].list_lock);
    }
    DeleteCriticalSection(&zeroList_head.list_lock);
    DeleteCriticalSection(&modifiedList_head.head_lock);
    DeleteCriticalSection(&standbyList_head.head_lock);

    for (int i = 0; i < NUMBER_OF_PHYSICAL_PAGES; i++) {
        ULONG64 frame = physical_page_numbers[i];
        DeleteCriticalSection(&physical_slots[frame].lock);
    }
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        DeleteCriticalSection(&pte_regions[i].lock);
    }
    free(pte_regions);
    pte_regions = NULL;

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree(VA_SPACE, 0, MEM_RELEASE);
    return;
    
}

VOID
main(
    int argc,
    char** argv
)
{
    full_virtual_memory_test();
    return;
}
