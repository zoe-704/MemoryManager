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
TRIM_PROFILE g_trim_prof;   // trimmer sub-phase breakdown (see print_mm_stats)
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
// Paired with g_qpc_freq to convert deltas to time and callers subtract the two
// QPC() samples and hands the delta to stat_add
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

// Trimmer sub-phase profiling hooks
#if STATISTICS
#define TPROF_NOW()       QPC()
#define TPROF_ADD(f, t0)  InterlockedAdd64(&g_trim_prof.f, QPC() - (t0))
#define TPROF_INC(f)      InterlockedIncrement64(&g_trim_prof.f)
#else
#define TPROF_NOW()       (0)
#define TPROF_ADD(f, t0)  ((void)(t0))
#define TPROF_INC(f)      ((void)0)
#endif

// Convert raw QPC ticks to ms using process-wide frequency
// g_qpc_freq must be set once (QueryPerformanceFrequency) before any stats print since guard returns 0.0
static DOUBLE
ms(LONG64 ticks) {
    return g_qpc_freq.QuadPart ? (double)ticks * 1000.0 / (double)g_qpc_freq.QuadPart : 0.0;
}



// Allocate and manage each thread's allocated scratch pages in temp_va_base
__declspec(thread) ULONG stage_cursor = 0;

// Each thread owns a non-overlapping THREAD_SCRATCH_PAGES pages chunk of shared temp_va_base to avoid collisions
// Asserts thread_index must be set bc the __declspec(thread) default -1 would compute negative offset
static __forceinline PULONG_PTR
thread_scratch_base(VOID)
{
    ASSERT(thread_index >= 0 && thread_index < NUM_THREADS);
    return (PULONG_PTR)((char*)temp_va_base + (SIZE_T)thread_index * THREAD_SCRATCH_PAGES * PAGE_SIZE);
}

// Single-page sub-region used to zero a frame
static __forceinline PULONG_PTR
thread_scrub_slot(VOID) 
{ 
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)SLOT_SCRUB * PAGE_SIZE); 
}

// Base of WRITE_BATCH-page window used by write_modified_list and zero_thread
// Batch-map a run of frames for a single map/copy/unmap
static __forceinline PULONG_PTR
thread_write_base(VOID) 
{ 
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)OFF_WRITE * PAGE_SIZE); 
}

// Base of staging ring used on the hard-fault disc-restore path to map frames one slot at a time
static __forceinline PULONG_PTR
thread_stage_base(VOID)
{
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)OFF_STAGE * PAGE_SIZE);
}

// Unmap every slot filled since the last flush in one syscall
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

// Claim the next slot and map the caller's frame into it
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

// Claim `d` contiguous stage slots, map the frames into them with a syscall, returns the base VA of the run
// Used by batched hard-fault prefetch to stage a run's disc reads
// Frames stay mapped until next stage_ring_flush
PULONG_PTR
stage_ring_map_batch(ULONG_PTR* frames, ULONG d)
{
    if (d == 0) return NULL;
    if (stage_cursor + d > STAGE_RING_PAGES) {
        stage_ring_flush();
    }
    PULONG_PTR base = (PULONG_PTR)((char*)thread_stage_base() + (SIZE_T)stage_cursor * PAGE_SIZE);
    if (MapUserPhysicalPages(base, d, frames) == FALSE) {
        printf("stage_ring_map_batch: map %lu frames failed, err %lu\n", d, GetLastError());
        DebugBreak();
        return NULL;
    }
    stage_cursor += d;
    return base;
}



// Region active-list and region-age index

// Highest age with a non-empty active bucket or NONE (caller holds region's lock)
ULONG64
region_oldest_age(PPTE_REGION region)
{
    for (int a = AGES - 1; a >= 0; a--) {
        if (region->age_counts[a] != 0) return (ULONG64)a;
    }
    return AGES;
}

// Move the region to the region-age list matching its current oldest age (caller holds region's lock)
VOID
region_rebucket(PPTE_REGION region)
{
    ULONG64 want = region_oldest_age(region);
    if (want == region->age_list_number) return;

    EnterCriticalSection(&region_age_lock);
    if (region->age_list_number != AGES) {
        RemoveEntryList(&region->age_link);
        region_age_lists[region->age_list_number].count--;
    }
    if (want != AGES) {
        InsertTailList(&region_age_lists[want].head, &region->age_link);
        region_age_lists[want].count++;
    }
    region->age_list_number = want;
    LeaveCriticalSection(&region_age_lock);
}

// Put a just-activated frame onto its region's age bucket (caller holds region's lock)
// PFN is exclusively owned or PFN's lock is held
VOID
region_add_active(PPTE_REGION region, pfn_metadata* pfn, ULONG64 age)
{
    // Must be NULL (frame just came off free/zero/standby)
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

// Trim one region and claim frames off oldest active buckets and puh onto modified list (caller holds region's lock)
// Once RemoveEntryList, no other path can reach it
// Unmap the whole batch before publishing so disc writer doesn't see still-mapped frame
static VOID
trim_region(PPTE_REGION region, ULONG64 oldest, int* batch_count, INT batch_size, ULONG* trimmed)
{
    PULONG_PTR unmap_vas[MAX_TRIM_PAGES];
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES];
    ULONG n = 0;

    // Step 1: claim victims off the oldest buckets and flip PTE to transition
    LONG64 t_claim = TPROF_NOW();
    for (int a = (int)oldest; a >= 0 && *batch_count < batch_size && n < MAX_TRIM_PAGES; a--) {
        while (!IsListEmpty(&region->active_age_lists[a]) &&
               *batch_count < batch_size && n < MAX_TRIM_PAGES) {
            PLIST_ENTRY e = RemoveHeadList(&region->active_age_lists[a]);
            pfn_metadata* pfn = get_pfn_from_PListEntry(e);
            // Confirm that node on the active list is a real active frame with a PTE
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
            // Mark as off the active list and in-transit
            pfn->links.Flink = NULL; 
            pfn->links.Blink = NULL;
            region->age_counts[a]--;
            if (region->active_page_count > 0) region->active_page_count--;

            EnterCriticalSection(&pfn->lock);
            PPTE pte = pfn->pte; 
            PULONG_PTR victim_va = get_va_from_pte(pte);
            set_pte_transition(pte, pfn->frame_number);
            pfn->list_type = LIST_NONE;   // off active but not yet on modified
            LeaveCriticalSection(&pfn->lock);

            unmap_vas[n] = (PVOID)((ULONG_PTR)victim_va & ~(PAGE_SIZE - 1));
            unmap_pfns[n] = pfn;
            n++;
            (*batch_count)++;
        }
    }

    if (n == 0) { TPROF_ADD(claim_ticks, t_claim); return; }
    TPROF_ADD(claim_ticks, t_claim);
    TPROF_INC(batch_calls);

    // Step 2: batch scatter-unmap before publishing since victim VAs are scattered
    LONG64 t_unmap = TPROF_NOW();
    if (MapUserPhysicalPagesScatter(unmap_vas, (ULONG_PTR)n, NULL) == FALSE) {
        printf("trim: batch unmap failed, n=%lu\n", n);
        DebugBreak();
    }
    TPROF_ADD(unmap_ticks, t_unmap);

    // Step 3: publish onto modified under each frame's lock
    LONG64 t_pub = TPROF_NOW();
    for (ULONG k = 0; k < n; k++) {
        pfn_metadata* pfn = unmap_pfns[k];
        EnterCriticalSection(&pfn->lock);
        pfn->list_type = LIST_MODIFIED;
        RWListInsertTail(&modifiedList_head, pfn);
        LeaveCriticalSection(&pfn->lock);
        (*trimmed)++;
    }
    TPROF_ADD(publish_ticks, t_pub);

    // Region's oldest age may have dropped or be empty
    region_rebucket(region);
}

// Getting page candidates to trim by draining the oldest region-age lists first
// Each region is claimed off the region-age index and its oldest active frames are taken under region's lock
VOID
get_unmap_candidates_and_trim(int* batch_count, INT batch_size)
{
#if STATISTICS
    LONG64 t0 = QPC();
#endif
    ULONG trimmed = 0;

    for (int age = AGES - 1; age >= 0 && *batch_count < batch_size; age--) {
        // Bound to current region age list
        EnterCriticalSection(&region_age_lock);
        ULONG64 iterations = region_age_lists[age].count;
        LeaveCriticalSection(&region_age_lock);

        while (iterations-- > 0 && *batch_count < batch_size) {
            // Rotate head region to the tail
            EnterCriticalSection(&region_age_lock);
            if (IsListEmpty(&region_age_lists[age].head)) {
                LeaveCriticalSection(&region_age_lock);
                break;
            }
            PLIST_ENTRY e = RemoveHeadList(&region_age_lists[age].head);
            InsertTailList(&region_age_lists[age].head, e);
            PPTE_REGION region = CONTAINING_RECORD(e, PTE_REGION, age_link);
            LeaveCriticalSection(&region_age_lock);

            if (!TryEnterCriticalSection(&region->lock)) { TPROF_INC(region_tryfail); continue; }
            // Re-check it is still on this age list
            if (region->age_list_number == (ULONG64)age && region->active_page_count > 0) {
                TPROF_INC(region_enter);
                trim_region(region, (ULONG64)age, batch_count, batch_size, &trimmed);
            }
            LeaveCriticalSection(&region->lock);
        }
    }

    g_my_stats->trims += trimmed;
#if STATISTICS
    stat_add(&g_trim_stat, QPC() - t0, 0, trimmed);
#else
    InterlockedAdd64(&g_trim_stat.pages, trimmed); 
#endif
    return;
}

// Shard for calling thread
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

// Total active pages across all PTE regions (pages on the age lists)
static ULONG64
active_pages_total(VOID)
{
    ULONG64 sum = 0;
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++)
        sum += pte_regions[i].active_page_count;
    return sum;
}


// Move up to CACHE_REFILL_BATCH pages from one shard into cache
ULONG
refill_free_cache(FREE_PAGE_CACHE* cache, int shard) {
    PLIST_HEAD sh = &freeList_shards[shard];
    if (!TryEnterCriticalSection(&sh->list_lock)) return 0;
    ULONG moved = 0;
    while (cache->count < FREE_CACHE_MAX && moved < CACHE_REFILL_BATCH && !IsListEmpty(&sh->entry)) {
        PLIST_ENTRY e = RemoveHeadList(&sh->entry);
        InterlockedDecrement64(&sh->list_count);
        pfn_metadata* pfn = get_pfn_from_PListEntry(e);
        // Null links here since frame is off the shard and exclusive
        pfn->links.Flink = NULL;
        pfn->links.Blink = NULL;
        cache->pages[cache->count++] = pfn;
        moved++;
    }
    LeaveCriticalSection(&sh->list_lock);
    if (moved) InterlockedAdd64(&cached_pages, (LONG64)moved);
    return moved;
}



// Disc

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
    return p;
}

// Lock-free disc-slot allocator with bitmap
// 
// One bit per slot (1 = used) and 64 in a LONG64 "row"
// Disc bitmap frees are atomic bit-clears
// Allocation by disc thread reserving slots into thread-local stash or claiming free bits 1 at atime
#define DISC_STASH_MAX (DISC_WRITE_BATCH * 2)
static __declspec(thread) ULONG64 disc_stash[DISC_STASH_MAX];
static __declspec(thread) int     disc_stash_count = 0;
static __declspec(thread) ULONG64 disc_scan_row = 0;   // resume the row scan here next refill

// Atomically claim one free slot's bit (TRUE if successful)
static BOOLEAN
disc_set_slot(ULONG64 slot)
{
    LONG64 mask = 1LL << (slot & 63);
    LONG64 old = InterlockedOr64(&disc_bitmap[slot >> 6], mask);
    if ((old & mask) != 0) return FALSE;   // already used
    InterlockedDecrement64(&g_disc_free_count);
    return TRUE;
}

// Reserve up to `target` free slots off the bitmap into the thread-local stash
static VOID
disc_refill_stash(int target)
{
    ULONG64 rows = disc_bitmap_rows;
    for (ULONG64 i = 0; i < rows && disc_stash_count < target; i++) {
        ULONG64 row = disc_scan_row;
        disc_scan_row = (disc_scan_row + 1 < rows) ? disc_scan_row + 1 : 0;

        LONG64 snap = disc_bitmap[row];
        if ((unsigned __int64)snap == ~0ULL) continue;   // full row: skip

        ULONG64 base = row * 64ULL;

        // Whole row free, so claim all 64
        if (snap == 0 && disc_stash_count + 64 <= DISC_STASH_MAX) {
            if (InterlockedCompareExchange64(&disc_bitmap[row], ~0LL, 0LL) == 0LL) {
                for (int b = 0; b < 64; b++) disc_stash[disc_stash_count++] = base + (ULONG64)b;
                InterlockedAdd64(&g_disc_free_count, -64);
                continue;
            }
            snap = disc_bitmap[row]; // rescan bits if lost the row
        }

        // Partial row, so claim each free bit individually
        for (int b = 0; b < 64 && disc_stash_count < DISC_STASH_MAX; b++) {
            if ((snap & (1LL << b)) != 0) continue; // used
            if (disc_set_slot(base + (ULONG64)b)) {
                disc_stash[disc_stash_count++] = base + (ULONG64)b;
            }
        }
    }
}

// Reserve one disc slot
// Hot: pop the stash OR Cold: refill the stash
ULONG64
get_disk_free_slots(VOID)
{
    InterlockedIncrement64(&disk_debug[0]);

    if (disc_stash_count == 0) {
        disc_refill_stash(DISC_WRITE_BATCH);
        if (disc_stash_count == 0) return (ULONG64)-1;   // disc full
    }

    ULONG64 slot = disc_stash[--disc_stash_count];
    InterlockedIncrement64(&disk_debug[1]);
    ASSERT(slot < disc_page_count);
    return slot;
}

// Any thread can free a disc slot with atomic bit-clear
VOID
return_disk_free_slots(ULONG64 slot)
{
    ASSERT(slot < disc_page_count);
    LONG64 clear_mask = ~(1LL << (slot & 63));
    LONG64 old = InterlockedAnd64(&disc_bitmap[slot >> 6], clear_mask);
    ASSERT((old & ~clear_mask) == ~clear_mask);   // the bit must have been set (in use)
    InterlockedIncrement64(&g_disc_free_count);
}

// Count of free disc slots excluding stashed ones
static ULONG64
disc_free_pages(VOID)
{
    LONG64 f = InterlockedOr64(&g_disc_free_count, 0);
    if (f < 0) f = 0;
    return (ULONG64)f > disc_page_count ? disc_page_count : (ULONG64)f;
}

static __forceinline double 
clampd(double v, double lo, double hi) 
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Age a slice of regions by clearing PTE access bit and moving active frame between region's age buckets
// Runs under region's lock
VOID
AgeSweep(ULONG64 regions_to_sweep)
{
    InterlockedIncrement64(&tick_call);
    for (ULONG64 n = 0; n < regions_to_sweep; n++) {
        ULONG64 i = age_cursor;
        age_cursor = (age_cursor + 1) % NUM_PTE_LOCKS;

        PPTE_REGION region = &pte_regions[i];
        // TryEnter since fault thread can hold this region lock across a disc memcpy
        // Clock hand comes back to it on the next sweep
        if (!TryEnterCriticalSection(&region->lock)) continue;
        if (region->active_page_count == 0) {
            LeaveCriticalSection(&region->lock);
            continue;
        }

        // Decide each frame's new bucket by PTE access bit but not moving it
        pfn_metadata* moved[PTES_PER_LOCK];
        BYTE from_age[PTES_PER_LOCK];
        BYTE to_age[PTES_PER_LOCK];
        ULONG m = 0;

        for (int a = 0; a < AGES && m < PTES_PER_LOCK; a++) {
            PLIST_ENTRY head = &region->active_age_lists[a];
            for (PLIST_ENTRY cur = head->Flink; cur != head && m < PTES_PER_LOCK; cur = cur->Flink) {
                pfn_metadata* pfn = get_pfn_from_PListEntry(cur);
                // Confirm active node is a real active frame
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
                PPTE pte = pfn->pte; 
                if (pte == NULL) continue;

                ULONG64 old = *(volatile ULONG64*)pte;
                PTE snap; snap.entire_contents = old;
                if (snap.hardware.valid != 1) continue;

                ULONG64 new_age;
                if (snap.hardware.accessed) new_age = 0;         // touched    -> hottest
                else if (a < AGES - 1) new_age = (ULONG64)a + 1; // untouched  -> colder
                else continue;                                   // cold+maxed -> leave it

                // Clear the access bit atomically or skip and pick it up next tick
                PTE upd = snap;
                upd.hardware.accessed = 0;
                if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
                        (LONG64)upd.entire_contents, (LONG64)old) != old) {
                    continue;
                }

                if (new_age != (ULONG64)a) {
                    moved[m] = pfn; from_age[m] = (BYTE)a; to_age[m] = (BYTE)new_age; m++;
                }
            }
        }

        // Apply the recorded bucket moves
        for (ULONG k = 0; k < m; k++) {
            RemoveEntryList(&moved[k]->links);
            region->age_counts[from_age[k]]--;
            InsertTailList(&region->active_age_lists[to_age[k]], &moved[k]->links);
            region->age_counts[to_age[k]]++;
        }

        // Region's oldest age may have changed
        region_rebucket(region);
        LeaveCriticalSection(&region->lock);
    }
}

// Per-thread stat slots on demand 
LONG64
total_hard_faults(VOID)
{
    LONG64 s = 0;
    for (int t = 0; t < NUM_THREADS; t++) s += thread_stats[t].hard_faults;
    return s;
}
LONG64
total_soft_faults(VOID)
{
    LONG64 s = 0;
    for (int t = 0; t < NUM_THREADS; t++) s += thread_stats[t].soft_faults;
    return s;
}

// Print statistics
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
    ULONG64 hard = (ULONG64)total_hard_faults();
    ULONG64 soft = (ULONG64)total_soft_faults();
    ULONG64 faults = hard + soft;

    printf("\n---------- STATISTICS ----------\n");
    printf("  FREE:     %6llu  (%.1f%%)\n", free_c, 100.0 * free_c / total);
    printf("  ACTIVE:   %6llu  (%.1f%%)\n", act_c,  100.0 * act_c / total);
    printf("  MODIFIED: %6llu  (%.1f%%)\n", mod_c,  100.0 * mod_c / total);
    printf("  STANDBY:  %6llu  (%.1f%%)\n", stby_c, 100.0 * stby_c / total);
    printf("  ZERO:     %6llu  (%.1f%%)\n", zero_c, 100.0 * zero_c / total);

    // Sampled racily so clamp at 0 
    // FREE+ACTIVE+MODIFIED+STANDBY+ZERO+IN-TRANSIT
    LONG64 intransit = (LONG64)total - (LONG64)(free_c + act_c + mod_c + stby_c + zero_c);
    if (intransit < 0) intransit = 0;
    printf("  IN-TRANSIT:%6lld  (%.1f%%)\n", intransit, 100.0 * (double)intransit / total);
    if (faults > 0) {
        printf("  HARD:     %6llu  (%.1f%%)\n", hard, 100.0 * hard / faults);
        printf("  SOFT:     %6llu  (%.1f%%)\n", soft, 100.0 * soft / faults);
    }
    printf("--------------------------------\n");

    // Per-thread exact breakdown
    printf("  thread |     hard |     soft | hard_disc | hard_zero |    waits |    trims |   writes |   zeroes\n");
    for (int t = 0; t < NUM_THREADS; t++) {
        THREAD_STATS* s = &thread_stats[t];
        if (s->hard_faults || s->soft_faults || s->trims || s->writes || s->zeroes) {
            printf("  %6d |%9lld |%9lld |%10lld |%10lld |%9lld |%9lld |%9lld |%9lld\n",
                t, s->hard_faults, s->soft_faults, s->hard_disc, s->hard_zero,
                s->fault_waits, s->trims, s->writes, s->zeroes);
        }
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

#if STATISTICS
    LONG64 total   = g_trim_stat.ticks;
    LONG64 claim   = g_trim_prof.claim_ticks;
    LONG64 unmap   = g_trim_prof.unmap_ticks;
    LONG64 publish = g_trim_prof.publish_ticks;
    LONG64 index   = total - claim - unmap - publish;
    if (index < 0) index = 0;
    double denom   = (total > 0) ? (double)total : 1.0;
    LONG64 tryok   = g_trim_prof.region_enter;
    LONG64 tryfail = g_trim_prof.region_tryfail;
    LONG64 tries   = tryok + tryfail;
    LONG64 batches = g_trim_prof.batch_calls ? g_trim_prof.batch_calls : 1;

    printf("Trim profile (active = %.1f ms; the rest of the run the trimmer is idle/level-capped):\n",
        ms(total));
    printf("  claim   (region+pfn locks, PTE flip) : %8.1f ms  (%.1f%%)\n", ms(claim),   100.0 * claim   / denom);
    printf("  unmap   (scatter-unmap syscall)      : %8.1f ms  (%.1f%%)\n", ms(unmap),   100.0 * unmap   / denom);
    printf("  publish (pfn lock + modified SRW)    : %8.1f ms  (%.1f%%)\n", ms(publish), 100.0 * publish / denom);
    printf("  index   (region-age idx + overhead)  : %8.1f ms  (%.1f%%)\n", ms(index),   100.0 * index   / denom);
    printf("  region TryEnter: %lld ok, %lld failed (%.1f%% contended) | batches=%lld avg %.1f pages/batch\n",
        tryok, tryfail, tries ? (100.0 * tryfail / (double)tries) : 0.0,
        g_trim_prof.batch_calls, (double)g_trim_stat.pages / batches);
#endif
}

// Move modified list entries to disk and push to standby
// Returns the number of frames drained
write_modified_list(VOID)
{
    PULONG_PTR batch_base = thread_write_base();
    ULONG_PTR frames[DISC_WRITE_BATCH];
    pfn_metadata* pfns[DISC_WRITE_BATCH];
    ULONG64 slots[DISC_WRITE_BATCH];
    ULONG count = 0;

    // Step 1: collect a batch of frames off the modified list
    // For an eligible frame, reserve it (disc slot + being_written) and remove it
    RW_LIST_CURSOR cur;
    for (pfn_metadata* pfn = RWListScanBegin(&modifiedList_head, &cur, TRUE);
         pfn != NULL && count < DISC_WRITE_BATCH;
         pfn = RWListScanNext(&cur)) {
        if (pfn->list_type == LIST_MODIFIED && !pfn->being_written) {
            ULONG64 slot = get_disk_free_slots();
            if (slot == (ULONG64)-1) {
                // Disc full: leave this frame on modified and stop collecting
                SetEvent(redoFault_event);
                break;
            }
            // Scan holds curr_lock (== this pfn->lock)
            pfn->being_written = 1;
            pfn_metadata* removed = RWListScanRemoveCurrent(&cur);
            if (removed == NULL) {
                // Successor contended so undo reservation and leave frame on modified
                pfn->being_written = 0;
                return_disk_free_slots(slot);
                continue;
            }
            pfns[count] = removed;
            frames[count] = removed->frame_number;
            slots[count] = slot;
            count++;
            unlock_pfn(removed);   // transferred lock and frame is off modified with being_written=1
        }
    }
    RWListScanEnd(&cur);

    // Step 2: batch map, 'c' amount of copies, batch unmap
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

        // Create 'c' amount of copies
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
        g_my_stats->writes += count;   // disc thread's slot
#if STATISTICS
        stat_add(&g_write_stat, t_map, t_copy_total, count);
#else
        InterlockedAdd64(&g_write_stat.pages, count);   
#endif
        
        // Step 3: commit to standby by PTE region
        // Re-validate each frame's PTE before inserting on standby list
        PPTE evict_pte_of[DISC_WRITE_BATCH];
        CRITICAL_SECTION* region_of[DISC_WRITE_BATCH];
        BOOLEAN committed[DISC_WRITE_BATCH];
        ULONG64 slot_to_free[DISC_WRITE_BATCH];

        // Pass 1: snapshot each frame's PTE (under its lock) and resolve its region lock
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

        // Pass 2: for each unhandled frame, take its region lock and process remaining frames that map to same region
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
                    // being_written frame is off every list and soft faults fail
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
                    slot_to_free[j] = slots[j];   // hand back after region lock is dropped
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

        // Pass 3: free poached slots after every region lock is dropped
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

    ULONG_PTR frames[DISC_WRITE_BATCH];
    pfn_metadata* pfns[DISC_WRITE_BATCH];
    ULONG count = 0;

    // Step 1: claim up to WRITE_BATCH frames off of free shards
    for (int s = 0; s < NUM_FREE_SHARDS && count < DISC_WRITE_BATCH; s++) {
        PLIST_HEAD sh = &freeList_shards[s];
        EnterCriticalSection(&sh->list_lock);
        while (count < DISC_WRITE_BATCH && !IsListEmpty(&sh->entry)) {
            PLIST_ENTRY free_e = RemoveHeadList(&sh->entry);
            InterlockedDecrement64(&sh->list_count);
            pfn_metadata* pfn = get_pfn_from_PListEntry(free_e);
            pfns[count] = pfn;
            frames[count] = pfn->frame_number;
            count++;
        }
        LeaveCriticalSection(&sh->list_lock);
    }

    // 'from_free' amount of pages came off free shards for zero list
	// Rest of the pages came off the standby list for free shards
    ULONG from_free = count;

    // Step 2: claim frames off the standby list 
    {
        RW_LIST_CURSOR cur;
        for (pfn_metadata* pfn = RWListScanBegin(&standbyList_head, &cur, TRUE);
             pfn != NULL && count < DISC_WRITE_BATCH;
             pfn = RWListScanNext(&cur)) {
            if (pfn->list_type == LIST_STANDBY && !pfn->being_written) {
                pfn_metadata* got = rwlist_scan_rescue_remove(&cur);
                if (got != NULL) {
                    pfns[count] = got;
                    frames[count] = got->frame_number;
                    count++;
                    unlock_pfn(got);   // transferred lock
                }
                // successor/region contended so skip
            }
        }
        RWListScanEnd(&cur);
    }
    if (count == 0) return 0;

    // Step 3: zero only the free-shard pages (map, memset, unmap)
    if (from_free > 0) {
        if (MapUserPhysicalPages(batch_base, from_free, frames) == FALSE) {
            printf("zero_thread: batch map failed, count=%lu\n", from_free);
            DebugBreak();
        }
        for (ULONG i = 0; i < from_free; i++) {
            memset((char*)batch_base + (SIZE_T)i * PAGE_SIZE, 0, PAGE_SIZE);
        }
        g_my_stats->zeroes += from_free;   // zero thread's slot
        if (MapUserPhysicalPages(batch_base, from_free, NULL) == FALSE) {
            printf("zero_thread: batch unmap failed, count=%lu\n", from_free);
            DebugBreak();
        }
    }

    // Step 4a: publish zeroed pages to zero list 
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

    // Step 4b: publish standby-rescued pages -> free shards
    for (int s = 0; s < NUM_FREE_SHARDS; s++) {
        PLIST_HEAD sh = &freeList_shards[s];
        EnterCriticalSection(&sh->list_lock);
        for (ULONG i = from_free + s; i < count; i += NUM_FREE_SHARDS) {
            pfns[i]->is_zero = 0;                       // free shards holds garbage
            pfns[i]->pte = NULL;
            pfns[i]->disc_index = INVALID_DISC_SLOT;
            pfns[i]->list_type = LIST_NONE;             // exclusively owned here since off every list
            pfns[i]->owner_thread_id = 0;               // release so consumer re-claims
            InsertTailList(&sh->entry, &pfns[i]->links);
            InterlockedIncrement64(&sh->list_count);
        }
        LeaveCriticalSection(&sh->list_lock);
    }
    SetEvent(redoFault_event);   // pages available
    return count;
}

// Resolve a fault at VA and retry same VA after
// run_len does hard-fault run prefetch (pages remaining in the current run)
VOID handle_page_fault(PVOID arbitrary_va, PPTE pte, ULONG64 run_len) {
    PTE snap;
    *(ULONG64*)&snap = *(volatile ULONG64*)pte;
    PPTE_REGION region_struct = get_pte_region(pte);
    PVOID page_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

    // Check 1: another thread may have already resolved this fault
    if (snap.hardware.valid == 1) {
        return;
    }

    // Check 2: transition PTE soft fault (rescue from modified/standby)
    else if (snap.transition.valid == 0 && snap.transition.transition == 1) {
        handle_soft_fault(arbitrary_va, pte, region_struct, page_aligned);
    }

    // Check 3: new/disc PTE
    else if (snap.hardware.valid == 0 && snap.transition.transition == 0) {
#if FAULT_RUN_PREFETCH
        // Prefetch the rest of this contiguous run in one batch 
        if (run_len > 0) {
            handle_hard_fault_run(arbitrary_va, pte, region_struct, page_aligned, run_len);
            return;
        }
#endif
        handle_hard_fault(arbitrary_va, pte, region_struct, page_aligned);
    }

    // Check 4: unexpected change to PTE so try same VA again
    else {
        return;
    }
}
DWORD WINAPI
page_fault_thread_random (PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;
    g_my_stats = &thread_stats[thread_index];   // thread's stats slot
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

    // Random accesses
    while (i < runtime) {
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

            // If the page isn't blank (0), it must match our VA
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
            // Redo this iteration against the same VA to confirm the mapping
            i--;
            fault_resolution = FALSE;
            handle_page_fault(arbitrary_va, pte, 0);
        }
        // Fault does not occur
        else {
            pte_set_accessed(pte);  // reuse the PTE
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
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot
    THREAD_RNG_STATE thread_rng;
    SeedRng(&thread_rng);

    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Page fault thread nonrandom %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    // Locality state
    ULONG64 total_pages = virtual_address_size_in_unsigned_chunks / (PAGE_SIZE / sizeof(ULONG_PTR));

    // Bound the working set so trimmed pages are still on standby when revisited
    ULONG64 cold_span = total_pages / WORKING_SET_DIVISOR;
    if (cold_span == 0) {
        cold_span = total_pages;
    }

    // Each thread gets its own slice so threads don't serialize on the same region locks
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
        page_faulted = FALSE;

        // Only pick a new VA when the previous one was fully resolved
        if (fault_resolution == TRUE) {

            if (run_left == 0) {
                ULONG64 r = GetNextRandom(&thread_rng);

                if (hot_count > 0 && (r % REVISIT_CHANCE) == 0) {
                    // REVISIT: re-touch the same base and length as before, so those pages are still valid or on modified/standby (soft faults)
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
                        : slice;                       // rare: whole slice
                    if (span == 0) span = 1;
                    base_page = slice_lo + (GetNextRandom(&thread_rng) % span);
                    run_left = MIN_RUN_PAGES +
                        (GetNextRandom(&thread_rng) % (MAX_RUN_PAGES - MIN_RUN_PAGES));
                }

                // Clamp to the end of the VA space
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

                // Stamp as most recent so repeated revisits keep a small set hot
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
            // Redo this iteration against the same VA but do not advance the run
            // Pass run_left so the hard-fault path can prefetch the rest of this contiguous run in one batch
            i--;
            fault_resolution = FALSE;
            handle_page_fault(arbitrary_va, pte, run_left);
        }
        else {
            pte_set_accessed(pte);   // reuse the PTE already derived above
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
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot
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

    // Level controller to hold the ready pool (free+standby+zero) at POOL_HIGH_WATER by trimming proportionally to deficit
    for (;;) {
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        ULONG64 ready = free_pages_total()
            + zeroList_head.list_count
            + standbyList_head.list_count;

        // Keep ready pool at POOL_HIGH_WATER 
        if (ready >= POOL_HIGH_WATER) {
            HANDLE waits[2] = { startTrim_event, shutdown_event };
            if (WaitForMultipleObjects(2, waits, FALSE, 5) == WAIT_OBJECT_0 + 1) break;
            continue;
        }

        // Prevent piling onto an already-full modified list
        if (modifiedList_head.list_count > MODIFIED_HIGH_WATER) {
            SetEvent(modifiedReady_event);
            if (WaitForSingleObject(shutdown_event, 1) == WAIT_OBJECT_0) break;
            continue;
        }

        // Trim  toward the setpoint
        LONG64 batch = (LONG64)(POOL_HIGH_WATER - ready);
        if (batch < MIN_TRIM_BATCH) batch = MIN_TRIM_BATCH;
        if (batch > MAX_TRIM_PAGES) batch = MAX_TRIM_PAGES;

        count = 0;
        get_unmap_candidates_and_trim(&count, (INT)batch);

        // Nothing trimmable so wait
        if (count == 0 && WaitForSingleObject(shutdown_event, 1) == WAIT_OBJECT_0) break;
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
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot

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

        // Drain the modified list by progress and each frame returns how many franes it committed
        for (;;) {
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
            if (write_modified_list() == 0) break;
        }
    }
    while (write_modified_list() > 0) { /* drain until empty */ }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("Disc thread total execution time: %.2f ms\n", elapsed_ms);

    return 0;
}

// thread type 4: aging 
DWORD WINAPI
age_thread(PVOID parameter)
{
    thread_index = (int)(ULONG_PTR)parameter;
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot

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
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot

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

        LONG64 hard = total_hard_faults();
        LONG64 soft = total_soft_faults();
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

        // Ready pages are immediately usable for trim counts
        ULONG64 ready = free_pages_total()
            + zeroList_head.list_count
            + standbyList_head.list_count;
        ULONG64 in_flight = modifiedList_head.list_count;
        ULONG64 supply = ready + in_flight;
        double remaining_s = (consume_rate > 0.0) ? (double)supply / consume_rate : 1e9;

        // Writer is behind so pause trimming until it drains
        LONG64 writer_behind = (in_flight > MODIFIED_HIGH_WATER);

        // Dynamic aging
        // steady-state/idle: set baseline target sweep time 
        // otherwise: calculate time to sweep over whole set at current rate
        double valid_count = (double)active_pages_total();

        double T_sweep_ms;
        if (consume_rate < 1.0) {
            T_sweep_ms = AGE_TSWEEP_BASELINE_MS;   // idle: keep ages fresh & differentiated
        }
        else {
            // V/C = time to churn the whole valid set at current pressure
            T_sweep_ms = (valid_count / (AGE_ALPHA * consume_rate)) * 1000.0;
            if (ready < LOW_FREE_PAGE_THRESHOLD) T_sweep_ms *= 0.5;  // pool draining sharpen ages faster
            T_sweep_ms = clampd(T_sweep_ms, AGE_TSWEEP_MIN_MS, AGE_TSWEEP_BASELINE_MS);
        }

        LONG64 rpt = (LONG64)(NUM_PTE_LOCKS * (double)AGE_TICK_MS / T_sweep_ms);
        InterlockedExchange64(&g_age_regions_per_tick, rpt < 1 ? 1 : rpt);

        // Dynamic trimming
        // consume_rate > trim_rate so max trimming OR balance with consumption rate
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
        // Integrity sweep once per tick to catch list corrpution
        check_concurrent_list(&standbyList_head, LIST_STANDBY, "standby");
        check_concurrent_list(&modifiedList_head, LIST_MODIFIED, "modified");
#endif

#if STATISTICS
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
    g_my_stats = &thread_stats[thread_index];   // this thread's private lock-free stats slot
    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Zero thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    // Zero thread's write region in the scratch section
    PULONG_PTR batch_base = thread_write_base();

    for (;;) {
        HANDLE waits[2] = { startZero_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        for (;;) {
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
            // Stop once the reserve is full
            if ((ULONG64)zeroList_head.list_count >= HIGH_ZERO_PAGE_THRESHOLD) break;
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

    // Create page fault threads
    HANDLE threads[NUM_THREADS] = { NULL };
    for (int i = 0; i < NUM_FAULT_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0, page_fault_thread_nonrandom, (PVOID)(ULONG_PTR)i, 0, NULL);
        //threads[i] = CreateThread(NULL, 0, page_fault_thread_random, (PVOID)(ULONG_PTR)i, 0, NULL);
    }
	// Create other threads: trim, disc, age, periodic, zero
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

    // Done with memory and free it 
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
