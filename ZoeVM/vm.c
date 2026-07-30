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
// pte_lock
//      ↓
// pfn_lock
//      ↓
// activeList_lock
// modifiedList_lock
// standbyList_lock
// freeList_lock
// 
//      ↓
// disc_lock
// 
// 
// 
// NEW ORDER: 
// region (pte) lock
//      ↓
// list lock(modified / standby / free)
//      ↓
// pfn lock(per - page)
//      ↓
// disc lock
// Back - edges(region acquired while holding a list) MUST use TryEnter and skip on failure.

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

// Convert raw QPC ticks to milliseconds using the process-wide frequency.
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
// (and zero_thread) to batch-map a run of frames for a single map/copy/unmap.
static __forceinline PULONG_PTR
thread_write_base(VOID) 
{ 
    return (PULONG_PTR)((char*)thread_scratch_base() + (SIZE_T)OFF_WRITE * PAGE_SIZE); 
}

// Base of this thread's staging ring, used on the hard-fault disc-restore path
// to map frames one slot at a time and amortize the unmap over many faults.
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
static PULONG_PTR
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

// Getters
// Getting pte regions
PPTE_REGION
get_pte_region(PPTE pte) 
{
    ULONG64 pte_index = pte - page_table;
    ASSERT(pte >= page_table);
    ASSERT(pte_index / PTES_PER_LOCK < NUM_PTE_LOCKS);
    return &pte_regions[pte_index / PTES_PER_LOCK];
}

// Getting pte region locks
CRITICAL_SECTION*
get_pte_lock(PPTE pte) 
{
    return &get_pte_region(pte)->lock;
}

// Gettinng PTE from VA
PPTE 
get_pte_from_va(PULONG_PTR va) 
{
    ULONG64 index = ((ULONG64)va - (ULONG64)VA_SPACE) / PAGE_SIZE;
    return page_table + index;
}

// Getting VA from PTE
PULONG_PTR
get_va_from_pte(PPTE pte) 
{
    ULONG64 pte_index = pte - page_table;
    PULONG_PTR page_va = VA_SPACE + (pte_index * (PAGE_SIZE / sizeof(ULONG_PTR)));
    return (PULONG_PTR)((ULONG_PTR)page_va & ~(PAGE_SIZE - 1)); 
}

// Getting lock guarding list entry's links
CRITICAL_SECTION*
get_entry_lock(PCONCURRENT_LIST_HEAD list, PLIST_ENTRY entry)
{
    if (entry == &list->entry) {
        return &list->head_lock;
    }
    return &get_pfn_from_PListEntry(entry)->lock;
}

// Try to lock pfn together with flink and blink using TryEnter
// Check that pfn is still between flink and blink
BOOLEAN
try_lock_pfn_window(PCONCURRENT_LIST_HEAD list, pfn_metadata* pfn)
{
    PLIST_ENTRY flink = pfn->links.Flink;
    PLIST_ENTRY blink = pfn->links.Blink;

    CRITICAL_SECTION* flink_lock = get_entry_lock(list, flink);
    CRITICAL_SECTION* blink_lock = get_entry_lock(list, blink);

    if (!TryEnterCriticalSection(flink_lock)) {
        return FALSE;
    }
    if (!TryEnterCriticalSection(&pfn->lock)) {
        LeaveCriticalSection(flink_lock);
        return FALSE;
    }
    if (!TryEnterCriticalSection(blink_lock)) {
        LeaveCriticalSection(&pfn->lock);
        LeaveCriticalSection(flink_lock);
        return FALSE;
    }

    // Re-check under the locks: the pfn may have been unlinked or moved before we got its lock
    if (pfn->links.Flink != flink || pfn->links.Blink != blink ||
        blink->Flink != &pfn->links || flink->Blink != &pfn->links) {
        LeaveCriticalSection(blink_lock);
        LeaveCriticalSection(&pfn->lock);
        LeaveCriticalSection(flink_lock);
        return FALSE;
    }
    return TRUE;
}

// Release a window taken by try_lock_pfn_window
VOID
unlock_pfn_window(PCONCURRENT_LIST_HEAD list, pfn_metadata* pfn)
{
    LeaveCriticalSection(get_entry_lock(list, pfn->links.Flink));
    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(get_entry_lock(list, pfn->links.Blink));
}

// Repurpose a standby frame in place: point its PTE at the disc copy and clear the frame's pte/disc
// Caller holds the frame's lock (window or exclusive)
BOOLEAN
rescue_standby_to_disc(pfn_metadata* pfn)
{
    PPTE old_pte = pfn->pte;
    if (old_pte == NULL) { DebugBreak(); return FALSE; }   // standby frame with no PTE: impossible
    CRITICAL_SECTION* region = get_pte_lock(old_pte);
    if (!TryEnterCriticalSection(region)) { return FALSE; }
    set_pte_disc(old_pte, pfn->disc_index);
    pfn->pte = NULL;
    pfn->disc_index = INVALID_DISC_SLOT;
    LeaveCriticalSection(region);
    return TRUE;
}

// Claim a frame just pulled off standby so no other path can hand it out
VOID
claim_rescued_pfn(pfn_metadata * pfn)
{
    pfn->state.list_type = LIST_NONE;   // off standby, in-transit
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev != 0) {
        printf("BUG: standby rescue handed out pfn %p already owned by tid %llu (I am tid %llu)\n",
            pfn, prev, tid);
        DebugBreak();
    }
}

// Lock a specific pfn for removal, escalating under contention. Returns TRUE with the frame
// locked and the srw held (mode in *exclusive); FALSE if the frame is contended (an inserter
// holds it), in which case nothing is held and the caller should retry.
// NOTE: the escalated path TryEnters the frame lock -- never blocks on it while holding the srw
// exclusive -- otherwise it would deadlock with inserters that hold pfn->lock then take srw.
BOOLEAN
lock_pfn_for_remove(PCONCURRENT_LIST_HEAD list, pfn_metadata* pfn, BOOLEAN* exclusive)
{
    AcquireSRWLockShared(&list->srw);
    for (int attempts = 0; attempts < WINDOW_RETRY_LIMIT; attempts++) {
        if (try_lock_pfn_window(list, pfn)) { *exclusive = FALSE; return TRUE; }
    }
    ReleaseSRWLockShared(&list->srw);          // fall in line behind the exclusive lock
    AcquireSRWLockExclusive(&list->srw);
    if (!TryEnterCriticalSection(&pfn->lock)) {
        ReleaseSRWLockExclusive(&list->srw);
        return FALSE;                          // contended by an inserter; caller retries
    }
    // Under exclusive we're the sole mutator, so the links are stable: verify the frame is still
    // stitched into a list before the caller unlinks it (matches the shared path's revalidation,
    // and guards against a frame that was removed but still carries a stale list_type).
    if (pfn->links.Blink->Flink != &pfn->links || pfn->links.Flink->Blink != &pfn->links) {
        LeaveCriticalSection(&pfn->lock);
        ReleaseSRWLockExclusive(&list->srw);
        return FALSE;
    }
    *exclusive = TRUE;
    return TRUE;
}

// Unlock current pfn
VOID
unlock_pfn_for_remove(PCONCURRENT_LIST_HEAD list, pfn_metadata* pfn, BOOLEAN exclusive)
{
    if (exclusive) {
        LeaveCriticalSection(&pfn->lock);
        ReleaseSRWLockExclusive(&list->srw);
    }
    else {
        unlock_pfn_window(list, pfn);
        ReleaseSRWLockShared(&list->srw);
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

// Getting page candidates to unmap and trim them
VOID
get_unmap_candidates_and_trim(int* batch_count, INT batch_size) 
{
#if STATISTICS
    LONG64 t0 = QPC();
#endif
    ULONG trimmed = 0;

    PULONG_PTR unmap_vas[MAX_TRIM_PAGES] = { NULL };
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES];

    // For each age, sweep every region
    for (int age = 7; age >= 0 && *batch_count < batch_size; age--) {
        for (ULONG64 i = 0; i < NUM_PTE_LOCKS && *batch_count < batch_size; i++) {
            PPTE_REGION region = &pte_regions[i];
            
            if (region->active_page_count == 0) continue;
            if (!TryEnterCriticalSection(&region->lock)) continue;
            // Recheck under lock
            if (region->age_counts[age] == 0) { 
                LeaveCriticalSection(&region->lock);
                continue;
            }

            ULONG64 start = i * PTES_PER_LOCK;
            ULONG64 end = min(start + PTES_PER_LOCK, num_ptes);
            for (ULONG64 j = start; j < end && *batch_count < batch_size; j++) {
                PPTE pte = page_table + j;
                // Atomic 8-byte snapshot
                PTE snap;
                *(ULONG64*)&snap = *(volatile ULONG64*)pte;

                // Don't trim invalid PTEs with the wrong age
                if (snap.hardware.valid != 1 || snap.hardware.age != (ULONG64)age) {
                    continue;
                }

                pfn_metadata* pfn = get_pfn_from_fn(snap.hardware.frame_number);
                if (!TryEnterCriticalSection(&pfn->lock)) {
                    continue;   // frame busy; skip this victim
                }

                // RE-CHECK 1: the PTE snapshot above was taken before we held the
                // list locks, so redo the read-decide now that we hold them
                // Disc writer could have already grabbed the page
                PTE fresh;
                *(ULONG64*)&fresh = *(volatile ULONG64*)pte;
                if (fresh.hardware.valid != 1 ||
                    fresh.hardware.frame_number != snap.hardware.frame_number || pfn->state.being_written) {
                    LeaveCriticalSection(&pfn->lock);
                    continue;
                }
     
                // Set pte to transition state and move lists
                PULONG_PTR victim_va = get_va_from_pte(pte);
                // Set pte to transition state
                set_pte_transition(pte, pfn->frame_number);

                ULONG64 trim_age = fresh.hardware.age;
                ASSERT(region->age_counts[trim_age] > 0);
                ASSERT(region->active_page_count > 0);
                region->active_page_count--;
                region->age_counts[trim_age]--;

                // Record VA and PFN in batch arrays
                unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)victim_va & ~(PAGE_SIZE - 1));
                unmap_pfns[*batch_count] = pfn;
                (*batch_count)++;

                // Move frame to modified list (active pages are on no list)
                pfn->state.list_type = LIST_MODIFIED;
                AcquireSRWLockExclusive(&modifiedList_head.srw);
                InsertTailList(&modifiedList_head.entry, &pfn->links);
                InterlockedIncrement64(&modifiedList_head.list_count);
                ReleaseSRWLockExclusive(&modifiedList_head.srw);

                LeaveCriticalSection(&pfn->lock);
                trimmed++;
            }
            LeaveCriticalSection(&region->lock);
        }
    }

    if (*batch_count > 0) 
    {
        // Batch unmap and unmark VAs as pending to be unmapped
        if (MapUserPhysicalPagesScatter(unmap_vas, (ULONG_PTR)*batch_count, NULL)) {
            for (int i = 0; i < *batch_count; i++) {
                EnterCriticalSection(&unmap_pfns[i]->lock);
                LeaveCriticalSection(&unmap_pfns[i]->lock);
            }
        }
        // Batch unmap failed and pages still mapped, so don't clear pending here
        else {
            DebugBreak();
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
static int
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

    // DEBUG: catch double-allocation at the source
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev_owner = InterlockedCompareExchange64((LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev_owner != 0) {
        printf("BUG: get_pfn_from_free handed out pfn %p already owned by tid %llu (I am tid %llu)\n", pfn, prev_owner, tid);
        DebugBreak();
    }
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
    pfn_metadata* result = NULL;
    BOOLEAN exclusive = FALSE;

    AcquireSRWLockShared(&standbyList_head.srw);
    PLIST_ENTRY entry = standbyList_head.entry.Flink;

    // Go through the entire standby list
    while (entry != &standbyList_head.entry) {
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);

        // Lock this frame: shared window with retries, else fall in line under the exclusive srw
        if (!exclusive) {
            int attempts = 0;
            while (attempts < WINDOW_RETRY_LIMIT && !try_lock_pfn_window(&standbyList_head, pfn)) {
                attempts++;
            }
            if (attempts == WINDOW_RETRY_LIMIT) {
                ReleaseSRWLockShared(&standbyList_head.srw);
                AcquireSRWLockExclusive(&standbyList_head.srw);
                exclusive = TRUE;
                entry = standbyList_head.entry.Flink;
                // restart walk with plain locks
                continue;
            }
        }
        else {
            EnterCriticalSection(&pfn->lock);
        }
        PLIST_ENTRY next = pfn->links.Flink;   // stable: we hold pfn->lock

        if (pfn->state.list_type == LIST_STANDBY && !pfn->state.being_written && rescue_standby_to_disc(pfn)) {
            RemoveEntryList(&pfn->links);
            InterlockedDecrement64(&standbyList_head.list_count);
            claim_rescued_pfn(pfn);
            result = pfn;
            if (exclusive) LeaveCriticalSection(&pfn->lock);
            else unlock_pfn_window(&standbyList_head, pfn);
            break;
        }

        if (exclusive) LeaveCriticalSection(&pfn->lock);
        else unlock_pfn_window(&standbyList_head, pfn);
        entry = next;
    }

    if (exclusive) ReleaseSRWLockExclusive(&standbyList_head.srw);
    else ReleaseSRWLockShared(&standbyList_head.srw);
    return result;
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

// Move up to FREE_REFILL_BATCH pages from one shard into cache
static ULONG
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

// Setters
// Set PTE to its valid state
VOID
set_pte_valid(PPTE pte, ULONG64 frame_number, ULONG64 age)
{
    PTE snapshot;
   // ZS use in other places read not necessary here
   // snapshot.entire_contents = ReadULong64NoFence(&pte->entire_contents);
    snapshot.entire_contents = 0;
   
    snapshot.hardware.valid = 1;
    snapshot.hardware.accessed = 1; // freshly faulted-in page counts as accessed but set 0 to start as cold
    snapshot.hardware.age = 0; // ZS only age?
    snapshot.hardware.frame_number = frame_number;
    snapshot.hardware.reserved = 0;

    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
}

// Zero out the PTE
VOID
set_pte_invalid(PPTE pte)
{
    WriteULong64NoFence((PLONG64)&pte->entire_contents, 0);
}

// Set PTE to its transistion state
VOID
set_pte_transition(PPTE pte, ULONG64 frame_number) 
{
    PTE snapshot;
    snapshot.entire_contents = 0;

    snapshot.transition.valid = 0;
    snapshot.transition.transition = 1;
    snapshot.transition.frame_number = frame_number;
    snapshot.transition.reserved = 0;
    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
}

// Set PTE to its disc state
VOID
set_pte_disc(PPTE pte, ULONG64 disc_index)
{
    PTE snapshot;
    snapshot.entire_contents = 0;

    snapshot.disc.valid = 0;
    snapshot.disc.transition = 0;
    snapshot.disc.disc = 1;
    snapshot.disc.disc_index = disc_index;
    snapshot.disc.reserved = 0;
    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
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

// Age stuff
VOID
pte_set_accessed(PPTE pte)
{
    for (;;) {
        ULONG64 old = *(volatile ULONG64*)pte;
        PTE s; *(ULONG64*)&s = old;
        if (s.hardware.valid != 1 || s.hardware.accessed == 1) break;
        PTE upd = s;
        upd.hardware.accessed = 1;
        if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
            *(LONG64*)&upd,
            (LONG64)old) == old) break;
    }
    return;
}

// Age pages
static __forceinline double 
clampd(double v, double lo, double hi) 
{
    return v < lo ? lo : (v > hi ? hi : v);
}

VOID
AgeListTick(VOID)
{
    InterlockedIncrement64(&tick_call);
    ULONG64 slice = NUM_PTE_LOCKS / AGE_SLICE_DIVISOR;
    if (slice == 0) slice = 1;

    // Walk through slice amount of pte regions starting from age_cursor
    for (ULONG64 n = 0; n < slice; n++) {
        ULONG64 i = age_cursor;
        age_cursor++;
        if (age_cursor >= NUM_PTE_LOCKS) age_cursor = 0;

        PPTE_REGION region = &pte_regions[i];
        EnterCriticalSection(&region->lock);
        if (region->active_page_count == 0) {
            LeaveCriticalSection(&region->lock);
            continue;
        }

        ULONG64 start = i * PTES_PER_LOCK;
        ULONG64 end = min(start + PTES_PER_LOCK, num_ptes);
        // Walk through each PTE in the region
        for (ULONG64 j = start; j < end; j++) {
            PPTE pte = page_table + j;

            ULONG64 old = *(volatile ULONG64*)pte;
            PTE snapshot;
            *(ULONG64*)&snapshot = old;
            if (snapshot.hardware.valid != 1) continue; // age only valid PTEs

            PTE updated = snapshot;
            ULONG64 from_age = snapshot.hardware.age;
            ULONG64 to_age;

            // Touched: decay one step toward hottest
            if (snapshot.hardware.accessed) {
                //to_age = (from_age > 0) ? from_age - 1 : 0;   // decay age?
                to_age = 0;
                updated.hardware.accessed = 0;
            }
            // Untouched: age up
            else if (from_age < 7) {
                to_age = from_age + 1;
            }
            else continue;   // cold and already maxed so nothing to do

            updated.hardware.age = to_age;

            // Publish first and buckets follow only if successful
            if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
                *(LONG64*)&updated,
                (LONG64)old) != old) {
                continue;   // someone changed the PTE so buckets untouched, retry next tick
            }

            if (to_age != from_age) {
                ASSERT(region->age_counts[from_age] > 0);
                region->age_counts[from_age]--;
                region->age_counts[to_age]++;
            }
        }
        LeaveCriticalSection(&region->lock);
    }
}

VOID 
AgeSweep(ULONG64 regions_to_sweep)      // was: slice computed from a #define
{
    InterlockedIncrement64(&tick_call);
    for (ULONG64 n = 0; n < regions_to_sweep; n++) {
        ULONG64 i = age_cursor;
        age_cursor = (age_cursor + 1) % NUM_PTE_LOCKS;
        if (age_cursor >= NUM_PTE_LOCKS) age_cursor = 0;

        PPTE_REGION region = &pte_regions[i];
        EnterCriticalSection(&region->lock);
        if (region->active_page_count == 0) {
            LeaveCriticalSection(&region->lock);
            continue;
        }

        ULONG64 start = i * PTES_PER_LOCK;
        ULONG64 end = min(start + PTES_PER_LOCK, num_ptes);
        // Walk through each PTE in the region
        for (ULONG64 j = start; j < end; j++) {
            PPTE pte = page_table + j;

            ULONG64 old = *(volatile ULONG64*)pte;
            PTE snapshot;
            *(ULONG64*)&snapshot = old;
            if (snapshot.hardware.valid != 1) continue; // age only valid PTEs

            PTE updated = snapshot;
            ULONG64 from_age = snapshot.hardware.age;
            ULONG64 to_age;

            // Touched: decay one step toward hottest
            if (snapshot.hardware.accessed) {
                //to_age = (from_age > 0) ? from_age - 1 : 0;
                to_age = 0;
                updated.hardware.accessed = 0;
            }
            // Untouched: age up
            else if (from_age < 7) {
                to_age = from_age + 1;
            }
            else continue;   // cold and already maxed so nothing to do

            updated.hardware.age = to_age;

            // Publish first and buckets follow only if successful
            if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
                *(LONG64*)&updated,
                (LONG64)old) != old) {
                continue;   // someone changed the PTE so buckets untouched, retry next tick
            }

            if (to_age != from_age) {
                ASSERT(region->age_counts[from_age] > 0);
                region->age_counts[from_age]--;
                region->age_counts[to_age]++;
            }
        }
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
    ULONG64 active_c = active_pages_total();
    ULONG64 mod_c = InterlockedOr64(&modifiedList_head.list_count, 0);
    ULONG64 stby_c = InterlockedOr64(&standbyList_head.list_count, 0);
    ULONG64 hard = InterlockedOr64(&hard_fault_count, 0);
    ULONG64 soft = InterlockedOr64(&soft_fault_count, 0);
    ULONG64 faults = hard + soft;

    printf("\n---------- STATISTICS ----------\n");
    printf("  FREE:     %6llu  (%.1f%%)\n", free_c, 100.0 * free_c / total);
    printf("  ACTIVE:   %6llu  (%.1f%%)\n", active_c, 100.0 * active_c / total);
    printf("  MODIFIED: %6llu  (%.1f%%)\n", mod_c, 100.0 * mod_c / total);
    printf("  STANDBY:  %6llu  (%.1f%%)\n", stby_c, 100.0 * stby_c / total);
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

// Handles soft faults
// Rescues pages in transition state from standby list and disc back to active memory
BOOLEAN
handle_soft_fault(PVOID arbitrary_va) 
{
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte); 
    CRITICAL_SECTION* region = get_pte_lock(pte);
    EnterCriticalSection(region);

    // Re-check under lock since state may have changed before we got here
    if (pte->hardware.valid == 1) {
        LeaveCriticalSection(region);
        return TRUE;    // already resolved by another thread
    }
    if (pte->transition.transition != 1) {
        LeaveCriticalSection(region);
        return FALSE;   // not a transition PTE anymore to handle
    }

    ULONG64 frame = pte->transition.frame_number;
    pfn_metadata* pfn = get_pfn_from_fn(frame);

    // Peek the list to pick a head (re-checked authoritatively under the frame lock below)
    ULONG64 lt = pfn->state.list_type;
    if (lt != LIST_MODIFIED && lt != LIST_STANDBY) {
        LeaveCriticalSection(region);
        return FALSE;
    }
    PCONCURRENT_LIST_HEAD list_head = (lt == LIST_STANDBY) ? &standbyList_head : &modifiedList_head;

    BOOLEAN exclusive;
    if (!lock_pfn_for_remove(list_head, pfn, &exclusive)) {
        // Frame is held by an inserter mid-transition; retry the fault from the top
        LeaveCriticalSection(region);
        return FALSE;
    }

    // Reread under frame lock
    if (pfn->state.list_type != lt || pfn->state.being_written) {
        // Frame moved lists between our read and the lock so bail
        unlock_pfn_for_remove(list_head, pfn, exclusive);
        LeaveCriticalSection(region);
        return FALSE;
    }
    // Set invalid disk slot if from standby list
    if (lt == LIST_STANDBY) {
        return_disk_free_slots(pfn->disc_index); // clears disc_slot_owner
        InterlockedIncrement64(&disk_debug[2]);
    }
    // Remove from list previously on
    RemoveEntryList(&pfn->links);
    ASSERT(list_head->list_count != 0);
    InterlockedDecrement64(&list_head->list_count);
    // Mark it off any list BEFORE dropping the lock. Otherwise a concurrent fault on the same
    // frame (PTE still in transition) sees the stale list_type, escalates, and tries to unlink
    // an already-removed page -> corrupts the list.
    pfn->state.list_type = LIST_NONE;
    unlock_pfn_for_remove(list_head, pfn, exclusive);

    // Align VA and map to frame
    PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
    if (MapUserPhysicalPages(page_aligned_va, 1, &frame) == FALSE) {
        printf("handle_page_fault: rescue remap failed\n");
        DebugBreak();
    }
#if DEBUG
    //validate_page_contents(page_aligned_va, "handle_soft_fault");
#endif

    // Reacquire lock to finish putting page back on active list
    EnterCriticalSection(&pfn->lock);

    // Edit PFN
    pfn->state.list_type = LIST_ACTIVE;
    pfn->pte = pte; // ZSPFN
    // Edit PTE
    set_pte_valid(pte, frame, 1);
    region_struct->active_page_count++;
    region_struct->age_counts[0]++;

    InterlockedIncrement64(&soft_fault_count);
    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(region);
    return TRUE;
}
// ZS map neighbors in mapphysicalpages call to increase iefficiency significantly
// based on access/age bit set when faulted on and when not faulted, check which range of pte in and see how that influences your next moves

// Handles hard faults
// PTE is either brand-nw or disc-backed (evicted)
// Need to acquire a physical frame, populate it (zero or read back from disc), and map it
BOOL
handle_hard_fault(PVOID arbitrary_va)
{
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte);
    CRITICAL_SECTION* region = get_pte_lock(pte);

    // STEP 1: get a free pfn (free > standby > trim)
    pfn_metadata* new_pfn = NULL;
    int attempts = 0;
    while (new_pfn == NULL) {
        EnterCriticalSection(region);

        // Page fault already resolved by someone else
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            return TRUE;
        }

        // Soft fault since someone else's rescue is in progress/going to start
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            BOOLEAN resolved = handle_soft_fault(arbitrary_va);
            if (resolved) return TRUE;
            continue;
        }

        // Hard fault path since not valid nor transition so need to get a page
        LeaveCriticalSection(region);


        new_pfn = get_free_pfn();
        if (new_pfn != NULL) break;

        SetEvent(startTrim_event);
        ResetEvent(redoFault_event);

        new_pfn = get_free_pfn();
        if (new_pfn != NULL) break;

        // Pool is empty so ask trimmer for pages
        /*
        // Synchronize redoFault event with the SetEvent in modified writer
        EnterCriticalSection(&standbyList_head.list_lock);
        BOOL no_pages = pool_is_empty();
        if (no_pages) ResetEvent(redoFault_event);
        LeaveCriticalSection(&standbyList_head.list_lock);
        if (!no_pages) continue;*/

        if (++attempts > 1000) {
            printf("handle_hard_fault: pool exhausted after %d retries\n", attempts);
            DebugBreak();
            return FALSE;
        }

        stage_ring_flush();

        HANDLE waits[2] = { redoFault_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) return FALSE;

    }

    PLIST_HEAD sh = &freeList_shards[free_shard_for_thread()];

    // STEP 2: if we got a page, map physical page to faulting VA
    EnterCriticalSection(region);
    if (new_pfn != NULL) {
        // PTE may have been resolved while pte_lock was dropped
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            // Another thread has restored this page so give frame back to free list
            EnterCriticalSection(&sh->list_lock);
            // ZS batch write this
            new_pfn->state.list_type = LIST_NONE;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            new_pfn->is_zero = 0;                       // free list holds garbage
            InsertHeadList(&sh->entry, &new_pfn->links);
            InterlockedIncrement64(&sh->list_count);
            LeaveCriticalSection(&sh->list_lock);
            SetEvent(startZero_event);
            return TRUE;
        }
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            // Return frame like above
            EnterCriticalSection(&sh->list_lock);
            // ZS batch write this
            new_pfn->state.list_type = LIST_NONE;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            new_pfn->is_zero = 0;                       // free list holds garbage
            InsertHeadList(&sh->entry, &new_pfn->links);
            InterlockedIncrement64(&sh->list_count);   
            LeaveCriticalSection(&sh->list_lock);
            return FALSE; // retry from top
        }
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;
        PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

        // STEP 3: restore from disc via the private staging slot, before exposing at the real VA
        if (from_disc) {
            PULONG_PTR temp_va = stage_ring_map(new_pfn->frame_number);
            if (temp_va == NULL) {
                LeaveCriticalSection(region);
                return FALSE;
            }

            memcpy(temp_va, (char*)disc + (old_disc_slot * PAGE_SIZE), PAGE_SIZE);

            ULONG_PTR first = *(PULONG_PTR)temp_va;
            if (first != 0 && (first & ~(PAGE_SIZE - 1)) != (ULONG_PTR)page_aligned_va) {
                printf("hard fault: slot %llu holds data for VA %p, expected %p\n",
                    old_disc_slot, (PVOID)first, page_aligned_va);
                DebugBreak();
            }
            /*
            if (MapUserPhysicalPages(temp_va, 1, NULL) == FALSE) {
                printf("could not unmap frame %llX from temp_va, err %lu\n",
                    new_pfn->frame_number, GetLastError());
                LeaveCriticalSection(region);
                DebugBreak();
                return FALSE;
            }*/

            return_disk_free_slots(old_disc_slot);
            InterlockedIncrement64(&disk_debug[3]);
        }

        // STEP 4: expose at the real VA
        if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
            printf("could not map VA %p to frame %llX, err %lu\n",
                arbitrary_va, new_pfn->frame_number, GetLastError());
            LeaveCriticalSection(region);
            DebugBreak();
            return FALSE;
        }

        // Only zero a frame we did NOT just overwrite from disc
        if (!from_disc && !new_pfn->is_zero) {
            memset(page_aligned_va, 0, PAGE_SIZE);
        }

#if DEBUG
        //validate_page_contents(page_aligned_va, "1695");
#endif

        // STEP 5: set to active state and update metadata
        // Insert onto active list and set as valid
        new_pfn->state.being_written = 0;
        new_pfn->state.list_type = LIST_ACTIVE;
        new_pfn->is_zero = 0;
        new_pfn->pte = pte;
        new_pfn->owner_thread_id = 0;  

        set_pte_valid(pte, new_pfn->frame_number, 1);
        region_struct->active_page_count++;
        region_struct->age_counts[0]++;

        InterlockedIncrement64(&hard_fault_count);
        LeaveCriticalSection(region);
    }
    return TRUE;
}

// Move modified list to disk and push to standby
VOID
write_modified_list(VOID) 
{
    PULONG_PTR batch_base = thread_write_base();
    ULONG_PTR frames[WRITE_BATCH];
    pfn_metadata* pfns[WRITE_BATCH];
    ULONG64 slots[WRITE_BATCH];
    ULONG count = 0;

    // Step 1: collect a batch off the modified list
    BOOLEAN exclusive = FALSE;
    AcquireSRWLockShared(&modifiedList_head.srw);
    PLIST_ENTRY entry = modifiedList_head.entry.Flink;

    while (count < WRITE_BATCH && entry != &modifiedList_head.entry) {
        // Grab page off modified list and mark as in transition
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);
        if (!exclusive) {
            int attempts = 0;
            while (attempts < WINDOW_RETRY_LIMIT && !try_lock_pfn_window(&modifiedList_head, pfn)) {
                attempts++;
            }
            if (attempts == WINDOW_RETRY_LIMIT) {
                ReleaseSRWLockShared(&modifiedList_head.srw);
                AcquireSRWLockExclusive(&modifiedList_head.srw);
                exclusive = TRUE;
                entry = modifiedList_head.entry.Flink;
                continue;
            }
        }
        else {
            EnterCriticalSection(&pfn->lock);
        }

        PLIST_ENTRY next = pfn->links.Flink;

        if (pfn->state.list_type == LIST_MODIFIED && !pfn->state.being_written) {
            ULONG64 slot = get_disk_free_slots();
            if (slot == (ULONG64)-1) {
                // disk full: leave this frame on modified and stop collecting
                if (exclusive) LeaveCriticalSection(&pfn->lock);
                else unlock_pfn_window(&modifiedList_head, pfn);
                SetEvent(redoFault_event);
                break;
            }
            pfn->state.being_written = 1;
            RemoveEntryList(&pfn->links);

            InterlockedDecrement64(&modifiedList_head.list_count);
            pfns[count] = pfn;
            frames[count] = pfn->frame_number;
            slots[count] = slot;
            count++;
        }

        if (exclusive) LeaveCriticalSection(&pfn->lock);
        else unlock_pfn_window(&modifiedList_head, pfn);
        entry = next;
    }
    if (exclusive) ReleaseSRWLockExclusive(&modifiedList_head.srw);
    else ReleaseSRWLockShared(&modifiedList_head.srw);

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
            PFN_STATE s; 
            s.whole = pfns[i]->state.whole;
            if (s.being_written == 0) { 
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
        
        // Step 3: commit to standby and clean up rescued
        for (ULONG i = 0; i < count; i++) {
            if (slots[i] == (ULONG64)-1) continue; // rescued in step 2 so slot already dealt with

            EnterCriticalSection(&pfns[i]->lock);
            if (pfns[i]->state.being_written == 0) {
                LeaveCriticalSection(&pfns[i]->lock);
                return_disk_free_slots(slots[i]);
                InterlockedIncrement64(&disk_debug[5]);
                continue;
            }
            pfns[i]->disc_index = slots[i];
            pfns[i]->state.being_written = 0;
            pfns[i]->state.list_type = LIST_STANDBY;

            AcquireSRWLockExclusive(&standbyList_head.srw);
            InsertTailList(&standbyList_head.entry, &pfns[i]->links);
            InterlockedIncrement64(&standbyList_head.list_count);
            ReleaseSRWLockExclusive(&standbyList_head.srw);
            LeaveCriticalSection(&pfns[i]->lock);
        }
        // Signal fault threads that standby has pages
        SetEvent(redoFault_event);
    }
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

    // Step 2: claim frames off of standby list via rescue-to-disc steps
    BOOLEAN exclusive = FALSE;
    AcquireSRWLockShared(&standbyList_head.srw);
    PLIST_ENTRY entry = standbyList_head.entry.Flink;
    // Standby list checks
    while (count < WRITE_BATCH && entry != &standbyList_head.entry) {
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);
        // Parallel access to standby list
        if (!exclusive) {
            int attempts = 0;
            while (attempts < WINDOW_RETRY_LIMIT && !try_lock_pfn_window(&standbyList_head, pfn)) {
                attempts++;
            }
            if (attempts == WINDOW_RETRY_LIMIT) {
                ReleaseSRWLockShared(&standbyList_head.srw);
                AcquireSRWLockExclusive(&standbyList_head.srw);
                exclusive = TRUE;
                entry = standbyList_head.entry.Flink;
                continue;
            }
        }
        else {
            EnterCriticalSection(&pfn->lock);
        }
        PLIST_ENTRY next = pfn->links.Flink;
        // Rescue standby pages
        if (pfn->state.list_type == LIST_STANDBY && !pfn->state.being_written &&
            rescue_standby_to_disc(pfn)) {
            RemoveEntryList(&pfn->links);
            InterlockedDecrement64(&standbyList_head.list_count);
            claim_rescued_pfn(pfn);
            pfns[count] = pfn;
            frames[count] = pfn->frame_number;
            count++;
        }

        if (exclusive) LeaveCriticalSection(&pfn->lock);
        else unlock_pfn_window(&standbyList_head, pfn);
        entry = next;
    }
    if (exclusive) ReleaseSRWLockExclusive(&standbyList_head.srw);
    else ReleaseSRWLockShared(&standbyList_head.srw);
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
            pfns[i]->state.list_type = LIST_NONE;
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
            pfns[i]->state.list_type = LIST_NONE;
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

            // Check 1: another thread may have already resolved this fault
            if (snap.hardware.valid == 1) {
                continue;
            }

            // Check 2: another fault thread may have already rescued this
            else if (snap.transition.valid == 0 && snap.transition.transition == 1) {
                if (!handle_soft_fault(arbitrary_va)) {
                    fault_resolution = FALSE;
                }
            }

            // Check 3: we will hard fault if pte from disc or completely new
            else if (snap.hardware.valid == 0 && snap.transition.transition == 0) {
                if (!handle_hard_fault(arbitrary_va)) {
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
            PPTE pte = get_pte_from_va(arbitrary_va);
            pte_set_accessed(pte);
            fault_resolution = TRUE;
        }
        if (i > 0 && i % 10000 == 0) {
            printf(". ");
            fflush(stdout);
        }
        i++;
    }

    stage_ring_flush();

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Thread execution time: %.2f ms\n", elapsed_ms);
    printf("Thread access iterations: %u\n", i);
    printf("==============================================\n\n");

    printf("page_fault_thread : finished accessing %u random virtual addresses\n", i);
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

            if (snap.hardware.valid == 1) {
                continue;
            }
            else if (snap.transition.valid == 0 && snap.transition.transition == 1) {
                if (!handle_soft_fault(arbitrary_va)) {
                    fault_resolution = FALSE;
                }
            }
            else if (snap.hardware.valid == 0 && snap.transition.transition == 0) {
                if (!handle_hard_fault(arbitrary_va)) {
                    fault_resolution = FALSE;
                }
            }
            else {
                continue;
            }
        }
        else {
            InterlockedIncrement64(&va_access_count);
            PPTE pte = get_pte_from_va(arbitrary_va);
            pte_set_accessed(pte);
            // Access succeeded so advance within the run
            cur_page++;
            run_left--;
            fault_resolution = TRUE;

        }
        if (i > 0 && i % 100000 == 0) {
            printf(". ");
            fflush(stdout);
        }
        i++;
    }
    stage_ring_flush();

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Thread execution time: %.2f ms\n", elapsed_ms);
    printf("Thread access iterations: %u\n", i);
    printf("==============================================\n\n");

    printf("page_fault_thread : finished accessing %u virtual addresses\n", i);
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

    printf("\n==============================================\n");
    printf("Trim thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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

        // Write modified 
        while (modifiedList_head.list_count > 0) {
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;
            write_modified_list();
        }
    }

    // Drain anything remaining before exit
    write_modified_list();

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Disc thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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

    printf("\n==============================================\n");
    printf("Age thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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
        if (modifiedList_head.list_count > MODIFIED_HIGH_WATER) SetEvent(modifiedReady_event);
        if (zeroList_head.list_count < LOW_ZERO_PAGE_THRESHOLD) SetEvent(startZero_event);

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

    printf("\n==============================================\n");
    printf("Periodic thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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

    printf("\n==============================================\n");
    printf("Zero thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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
    threads[NUM_FAULT_THREADS] = CreateThread(NULL, 0, trim_thread, (PVOID)(ULONG_PTR)8, 0, NULL);
    threads[NUM_FAULT_THREADS+1] = CreateThread(NULL, 0, disc_thread, (PVOID)(ULONG_PTR)9, 0, NULL);
    threads[NUM_FAULT_THREADS+2] = CreateThread(NULL, 0, age_thread, (PVOID)(ULONG_PTR)10, 0, NULL);
    threads[NUM_FAULT_THREADS+3] = CreateThread(NULL, 0, periodic_thread, (PVOID)(ULONG_PTR)11, 0, NULL);
    threads[NUM_FAULT_THREADS+4] = CreateThread(NULL, 0, zero_thread, (PVOID)(ULONG_PTR)12, 0, NULL);

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
