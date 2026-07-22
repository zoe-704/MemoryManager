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
// list lock(active / modified / standby / free)
//      ↓
// pfn lock(per - page)
//      ↓
// disc lock
// Back - edges(region acquired while holding a list) MUST use TryEnter and skip on failure.

#include "vm.h"

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
    LONG64 t0 = QPC();
    ULONG trimmed = 0;

    PULONG_PTR unmap_vas[MAX_TRIM_PAGES] = { NULL };
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES];

    // For each age, sweep every region
    for (int age = 7; age > 0 && *batch_count < batch_size; age--) {
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

                EnterCriticalSection(&activeList_head.list_lock);
                EnterCriticalSection(&modifiedList_head.list_lock);

                // RE-CHECK 1: the PTE snapshot above was taken before we held the
                // list locks, so redo the read-decide now that we hold them.
                PTE fresh;
                *(ULONG64*)&fresh = *(volatile ULONG64*)pte;
                if (fresh.hardware.valid != 1 ||
                    fresh.hardware.frame_number != snap.hardware.frame_number) {
                    LeaveCriticalSection(&modifiedList_head.list_lock);
                    LeaveCriticalSection(&activeList_head.list_lock);
                    continue;
                }

                // RE-CHECK 2: the frame must actually be on the active list, i.e. the
                // source list whose lock we hold. Anything else is in transit under
                // someone else's lock and is not ours to move.
                if (pfn->state.list_type != 1) {
                    LeaveCriticalSection(&modifiedList_head.list_lock);
                    LeaveCriticalSection(&activeList_head.list_lock);
                    continue;
                }

                // Disc writer already grabbed the page
                if (pfn->state.being_written) {
                    LeaveCriticalSection(&modifiedList_head.list_lock);
                    LeaveCriticalSection(&activeList_head.list_lock);
                    continue;
                }
                // Set pte to transition state and move lists
                PULONG_PTR victim_va = get_va_from_pte(pte);
                // Set pte to transition state
                set_pte_transition(pte, pfn->frame_number);
                region->active_page_count--;
                region->age_counts[fresh.hardware.age]--;

                // Record VA and PFN in batch arrays
                unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)victim_va & ~(PAGE_SIZE - 1));
                unmap_pfns[*batch_count] = pfn;
                (*batch_count)++;

                // Move frame from active to modified list
                RemoveEntryList(&pfn->links);
                InterlockedDecrement64(&activeList_head.list_count);

                pfn->state.list_type = 2;
                InsertTailList(&modifiedList_head.entry, &pfn->links);
                InterlockedIncrement64(&modifiedList_head.list_count);

                LeaveCriticalSection(&modifiedList_head.list_lock);
                LeaveCriticalSection(&activeList_head.list_lock);
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
    stat_add(&g_trim_stat, QPC() - t0, 0, trimmed);
    return;
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
    EnterCriticalSection(&freeList_head.list_lock);
    if (IsListEmpty(&freeList_head.entry)) {
        LeaveCriticalSection(&freeList_head.list_lock);
        return NULL;
    }
    // Get page off of free list
    PLIST_ENTRY entry = RemoveHeadList(&freeList_head.entry);
    InterlockedDecrement64(&freeList_head.list_count);
    LeaveCriticalSection(&freeList_head.list_lock);
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

// Rescue a free frame from standby list in its disc state
pfn_metadata*
get_pfn_from_standby(VOID)
{
    pfn_metadata* result = NULL;
    EnterCriticalSection(&standbyList_head.list_lock);
    PLIST_ENTRY standby_entry = standbyList_head.entry.Flink;

    // Go through the entire standby listf
    while (standby_entry != &standbyList_head.entry) {
        pfn_metadata* new_pfn = get_pfn_from_PListEntry(standby_entry);
        EnterCriticalSection(&new_pfn->lock);
        PLIST_ENTRY next = standby_entry->Flink;

        //ASSERT(new_pfn->state.list_type == 3);
        if (new_pfn->state.list_type != 3 || new_pfn->state.being_written) {
            LeaveCriticalSection(&new_pfn->lock);
            standby_entry = next;
            continue;                       // not cleanly on standby — skip it
        }

        PPTE old_pte = new_pfn->pte;
        if (old_pte != NULL) {
            CRITICAL_SECTION* region = get_pte_lock(old_pte);
            if (TryEnterCriticalSection(region)) {
                // Case B: different region but grabbed without blocking
                RemoveEntryList(&new_pfn->links);
                InterlockedDecrement64(&standbyList_head.list_count);

                set_pte_disc(old_pte, new_pfn->disc_index);
                new_pfn->pte = NULL;
                new_pfn->disc_index = INVALID_DISC_SLOT;

                LeaveCriticalSection(region);

                result = new_pfn;
                LeaveCriticalSection(&new_pfn->lock);
                break;
            }
        }
        // old_pte == NULL on a standby frame should not happen
        else {
            DebugBreak();
        }
        LeaveCriticalSection(&new_pfn->lock);
        standby_entry = next; // advance with saved pointer
    }

    // DEBUG: claim under lock so no other path can grab the same frame
    if (result != NULL) {
        result->state.list_type = 0; // no longer on standby list so in-transit to active
        ULONG64 tid = GetCurrentThreadId();
        ULONG64 prev = InterlockedCompareExchange64(
            (LONG64 volatile*)&result->owner_thread_id, tid, 0);
        if (prev != 0) {
            printf("BUG: get_pfn_from_standby handed out pfn %p already owned by tid %llu (I am tid %llu)\n",
                result, prev, tid);
            DebugBreak();
        }
    }
    LeaveCriticalSection(&standbyList_head.list_lock);
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
    snapshot.hardware.age = age;
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

// Interlocked reads to see if pool of free and stamdby pages is empty
BOOL
pool_is_empty(VOID) 
{
    return (InterlockedOr64(&freeList_head.list_count, 0) == 0 &&
        InterlockedOr64(&standbyList_head.list_count, 0) == 0);
}

// Zero a frame's physical contents via this thread's scratch VA
// Guarantees a frame placed on the free list holds no live data
// Caller must NOT hold the frame mapped at any other VA
VOID
scrub_frame(pfn_metadata* pfn)
{
    PULONG_PTR scratch = (PULONG_PTR)((char*)temp_va_base + ((SIZE_T)thread_index * PAGE_SIZE));
    if (MapUserPhysicalPages(scratch, 1, &pfn->frame_number) == FALSE) {
        printf("scrub_frame: could not map frame %llX to scratch\n", pfn->frame_number);
        DebugBreak();
        return;
    }
    memset(scratch, 0, PAGE_SIZE);
    if (MapUserPhysicalPages(scratch, 1, NULL) == FALSE) {
        printf("scrub_frame: could not unmap frame %llX from scratch\n", pfn->frame_number);
        DebugBreak();
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

// Return first free disc slot to fill
ULONG64
get_disk_free_slots(VOID)
{
    InterlockedIncrement64(&disk_debug[0]);
    EnterCriticalSection(&disc_stack_lock);
    if (disc_stack_top == 0) { // empty disc
        LeaveCriticalSection(&disc_stack_lock);
        return (ULONG64)-1;
    }
    disc_stack_top--;
    ULONG64 slot = disc_free_stack[disc_stack_top];
    InterlockedIncrement64(&disk_debug[1]);
    LeaveCriticalSection(&disc_stack_lock);

    ASSERT(slot < disc_page_count);
    ASSERT(disc_metadata[slot].isOccupied == FALSE);
    disc_metadata[slot].isOccupied = TRUE;
    return slot;
}

// Push back to stack and mark metadata as free
VOID
return_disk_free_slots(
    ULONG64 slot
) {
    ASSERT(slot < disc_page_count);
    PDISC_METADATA meta = &disc_metadata[slot];

    EnterCriticalSection(&disc_stack_lock);
    ASSERT(meta->isOccupied == TRUE);
    meta->isOccupied = FALSE;
    ASSERT(disc_stack_top < (LONG64)disc_page_count);   // never overflow the stack
    disc_free_stack[disc_stack_top] = slot;
    disc_stack_top++;
    LeaveCriticalSection(&disc_stack_lock);
    SetEvent(diskReady_event);
}

// Age stuff
BOOLEAN
PTE_WasAccessed(PPTE pte)
{
    return (BOOLEAN)pte->hardware.accessed;
}

VOID
PTE_SetAccessedBit(PPTE pte)
{
    pte->hardware.accessed = 1;
    return;
}

VOID
PTE_ClearAccessedBit(PPTE pte) 
{
    pte->hardware.accessed = 0;
    return;
}

// Age pages
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

            PTE snapshot;
            *(ULONG64*)&snapshot = *(volatile ULONG64*)pte;
            if (snapshot.hardware.valid != 1) continue; // age only valid PTEss

            if (snapshot.hardware.accessed) {
                snapshot.hardware.accessed = 0;         // touched and reset but keep age
            }
            else if (snapshot.hardware.age < 7) {
                region->age_counts[snapshot.hardware.age]--;
                snapshot.hardware.age++;                // untouched and age up
                region->age_counts[snapshot.hardware.age]++;
            }
            else continue;

            *(volatile ULONG64*)pte = *(ULONG64*)&snapshot;
        }
        LeaveCriticalSection(&region->lock);
    }
}

VOID
print_age_list_histogram (VOID)
{
    ULONG64 buckets[AGES] = { 0 };
    ULONG64 total_active = 0;
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        EnterCriticalSection(&pte_regions[i].lock);
        for (int age = 1; age <= 7; age++) {
            buckets[age] += pte_regions[i].age_counts[age];
        }
        LeaveCriticalSection(&pte_regions[i].lock);
    }

    printf("\n---------- AGE LIST HISTOGRAM ----------\n");
    for (int age = 1; age <= 7; age++) {
        printf("  age %d: %6llu pages\n", age, buckets[age]);
        total_active += buckets[age];
    }
    printf("  ------------------------------\n");
    printf("  total in age lists : %6llu\n", total_active);
    printf("  free list          : %6llu\n", freeList_head.list_count);
    printf("  active list        : %6llu\n", activeList_head.list_count);
    printf("  modified list      : %6llu\n", modifiedList_head.list_count);
    printf("  standby list       : %6llu\n", standbyList_head.list_count);
    printf("  tick call          : %6llu\n", tick_call);
    printf("-----------------------------------------\n\n");
}

VOID
print_statistics(VOID)
{
    ULONG64 total = NUMBER_OF_PHYSICAL_PAGES;
    ULONG64 free_c = InterlockedOr64(&freeList_head.list_count, 0);
    ULONG64 active_c = InterlockedOr64(&activeList_head.list_count, 0);
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

static __forceinline LONG64 QPC(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static __forceinline VOID stat_add(MM_STAT* s, LONG64 t, LONG64 tsub, LONG64 pages) {
    InterlockedIncrement64(&s->calls);
    InterlockedAdd64(&s->ticks, t);
    InterlockedAdd64(&s->ticks_sub, tsub);
    InterlockedAdd64(&s->pages, pages);
}

static DOUBLE ms(LONG64 ticks) {
    return (double)ticks * 1000.0 / (double)g_qpc_freq.QuadPart;
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

    // Read list_type to choose the lock
    PFN_STATE s0; s0.whole = pfn->state.whole;
    ULONG64 lt = s0.list_type;
    if (lt != 2 && lt != 3) {          // only modified/standby are rescuable here
        LeaveCriticalSection(region);
        return FALSE;
    }
    PLIST_HEAD list_head = (lt == 3) ? &standbyList_head : &modifiedList_head;
    EnterCriticalSection(&list_head->list_lock);

    // Reread under lock
    PFN_STATE s; s.whole = pfn->state.whole;
    if (s.list_type != lt) {
        // Frame moved lists between our read and the lock so bail
        LeaveCriticalSection(&list_head->list_lock);
        LeaveCriticalSection(region);
        return FALSE;
    }

    
    // Normal rescue from disc
    if (s.being_written) {
        //pfn->state.being_written = 0;
        LeaveCriticalSection(&list_head->list_lock);
        LeaveCriticalSection(region);
        return FALSE;
    }

    // Set invalid disk slot if from standby list
    if (lt == 3) {
        return_disk_free_slots(pfn->disc_index); // clears disc_slot_owner
        InterlockedIncrement64(&disk_debug[2]);
    }
    // Remove from list previously on
    RemoveEntryList(&pfn->links);
    ASSERT(list_head->list_count != 0);
    InterlockedDecrement64(&list_head->list_count);
    LeaveCriticalSection(&list_head->list_lock);

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
    EnterCriticalSection(&activeList_head.list_lock);
    EnterCriticalSection(&pfn->lock);

    // Edit PFN
    pfn->state.list_type = 1;
    pfn->pte = pte; // ZSPFN
    InsertTailList(&activeList_head.entry, &pfn->links);
    // Edit PTE
    set_pte_valid(pte, frame, 1);
    InterlockedIncrement64(&activeList_head.list_count);
    region_struct->active_page_count++;
    region_struct->age_counts[1]++;
    LeaveCriticalSection(&activeList_head.list_lock);

    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(region);
    InterlockedIncrement64(&soft_fault_count);
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

        HANDLE waits[2] = { redoFault_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) return FALSE;

    }

    // STEP 2: if we got a page, map physical page to faulting VA
    EnterCriticalSection(region);
    if (new_pfn != NULL) {
        // PTE may have been resolved while pte_lock was dropped
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            // Another thread has restored this page so give frame back to free list
            EnterCriticalSection(&freeList_head.list_lock);
            // ZS batch write this
            new_pfn->state.list_type = 0;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0; 
            InsertHeadList(&freeList_head.entry, &new_pfn->links);
            InterlockedIncrement64(&freeList_head.list_count);   
            LeaveCriticalSection(&freeList_head.list_lock);
            SetEvent(startZero_event);
            return TRUE;
        }
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            // Return frame like above 
            EnterCriticalSection(&freeList_head.list_lock);
            // ZS batch write this
            new_pfn->state.list_type = 0;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            InsertHeadList(&freeList_head.entry, &new_pfn->links);
            InterlockedIncrement64(&freeList_head.list_count);   
            LeaveCriticalSection(&freeList_head.list_lock);
            return FALSE; // retry from top
        }
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;

        PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
        PULONG_PTR temp_va = (PULONG_PTR)((char*)temp_va_base + ((SIZE_T)thread_index * WRITE_BATCH * PAGE_SIZE));

        // Step 3: clear the frame
        // Only zero a frame we're NOT about to overwrite from disc.
        // Disc restores memcpy the full PAGE_SIZE, so pre-zeroing is dead work.
        if (!from_disc && !new_pfn->is_zero) {
            scrub_frame(new_pfn);
        }

        // STEP 4: restore from disk if needed
        if (from_disc) {
            // Map the frame to a per-thread scratch VA
            if (MapUserPhysicalPages(temp_va, 1, &new_pfn->frame_number) == FALSE) {
                printf("could not map frame %llX to temp_va\n", new_pfn->frame_number);
                LeaveCriticalSection(region);
                DebugBreak();
                return FALSE;
            }

            // Get data from disc and copy to new page
            VOID* disc_address = (char*)disc + (old_disc_slot * PAGE_SIZE);
            memcpy(temp_va, disc_address, PAGE_SIZE);
            
            ULONG_PTR first = *(PULONG_PTR)temp_va;
            if (first != 0 && (first & ~(PAGE_SIZE - 1)) != (ULONG_PTR)page_aligned_va) {
                printf("hard fault: slot %llu holds data for VA %p, expected %p\n", old_disc_slot, (PVOID)first, page_aligned_va);
                DebugBreak();
            }

            // Unmap from temp before remapping elsewhere, frame can't be mapped at 2 VAs
            if (MapUserPhysicalPages(temp_va, 1, NULL) == FALSE) {
                printf("could not unmap frame %llX from temp_va\n", new_pfn->frame_number);
                LeaveCriticalSection(region);
                DebugBreak();
                return FALSE;
            }

            return_disk_free_slots(old_disc_slot); // Free up slot (clears disc_slot_owner)
            InterlockedIncrement64(&disk_debug[3]);
        }

        // Expose at the real VA 
        if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
            printf("could not map VA %p to page %llX\n", arbitrary_va, new_pfn->frame_number);
            LeaveCriticalSection(region);
            DebugBreak();
            return FALSE;
        }
#if DEBUG
        //validate_page_contents(page_aligned_va, "1695");
#endif DEBUG

        // STEP 5: set to active state and update metadata
        // Insert onto active list and set as valid
        EnterCriticalSection(&activeList_head.list_lock);
        new_pfn->state.being_written = 0;
        new_pfn->state.list_type = 1;
        new_pfn->is_zero = 0;
        new_pfn->pte = pte;
        new_pfn->owner_thread_id = 0;  
        InsertTailList(&activeList_head.entry, &new_pfn->links);

        set_pte_valid(pte, new_pfn->frame_number, 1);
        InterlockedIncrement64(&activeList_head.list_count);
        region_struct->active_page_count++;
        region_struct->age_counts[1]++;

        LeaveCriticalSection(&activeList_head.list_lock);
        LeaveCriticalSection(region);
    }
    InterlockedIncrement64(&hard_fault_count);
    return TRUE;
}

// Move modified list to disk and push to standby
VOID
write_modified_list(VOID) 
{
    PULONG_PTR batch_base = (PULONG_PTR)((char*)temp_va_base + ((SIZE_T)thread_index * WRITE_BATCH * PAGE_SIZE));
    ULONG_PTR frames[WRITE_BATCH];
    pfn_metadata* pfns[WRITE_BATCH];
    ULONG64 slots[WRITE_BATCH];
    ULONG count = 0;

    // Step 1: collect a batch off the modified list
    while (count < WRITE_BATCH) {
        EnterCriticalSection(&modifiedList_head.list_lock);
        if (IsListEmpty(&modifiedList_head.entry)) {
            // Soft faulter pulled all modified entries
            LeaveCriticalSection(&modifiedList_head.list_lock);
            break;
        }

        // Grab page off modified list and mark as in transition
        PLIST_ENTRY entry = RemoveHeadList(&modifiedList_head.entry);
        InterlockedDecrement64(&modifiedList_head.list_count);
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);
        ASSERT(pfn->state.being_written == 0);

        // Another thread owns it for write so restore
        if (pfn->state.being_written) {
            pfn->state.list_type = 2;
            InsertTailList(&modifiedList_head.entry, &pfn->links);
            InterlockedIncrement64(&modifiedList_head.list_count);
            LeaveCriticalSection(&modifiedList_head.list_lock);
            continue;
        }

        ASSERT(pfn->state.list_type == 2);
        ASSERT(pfn->pte != NULL);

        pfn->state.being_written = 1;
        LeaveCriticalSection(&modifiedList_head.list_lock);

        // Get disk slot info
        ULONG64 slot = get_disk_free_slots();
        if (slot == (ULONG64)-1) {
            // If disk is full undo transition and put it back on modified
            EnterCriticalSection(&modifiedList_head.list_lock);
            // Pfn has not been rescued yet
            pfn->state.being_written = 0;
            InsertHeadList(&modifiedList_head.entry, &pfn->links);
            InterlockedIncrement64(&modifiedList_head.list_count);
            LeaveCriticalSection(&modifiedList_head.list_lock);
            SetEvent(redoFault_event);
            break;
            //WaitForSingleObject(diskReady_event, INFINITE); // moved from disc thread
            //continue;
        }
        // Update metadata for batch map
        pfns[count] = pfn;
        frames[count] = pfn->frame_number;
        slots[count] = slot;
        count++;
    }

    // Step 2: batch map, c amount of copies, batch unmap
    if (count > 0) {
        LONG64 t0 = QPC();
        // Batch map
        if (MapUserPhysicalPages(batch_base, count, frames) == FALSE) {
            printf("batch map failed, count=%lu\n", count);
            DebugBreak();
        }
        LONG64 t_map = QPC() - t0;

        // Create c amount of copies
        for (ULONG i = 0; i < count; i++) {
            PFN_STATE s; s.whole = pfns[i]->state.whole;
            if (s.being_written == 0) { 
                // Rescued during collection so mark slot for return after unmap
                slots[i] = (ULONG64)-1;
                continue;
            }
            pfns[i]->disc_index = slots[i];

            // Memcpy data to disk
            PULONG_PTR src = (PULONG_PTR)((char*)batch_base + (i * PAGE_SIZE));
            LONG64 t1 = QPC();
            memcpy((BYTE*)disc + slots[i] * PAGE_SIZE, src, PAGE_SIZE);
            LONG64 t_copy = QPC() - t1;
        }
        // Batch unmap
        if (MapUserPhysicalPages(batch_base, count, NULL) == FALSE) {
            printf("batch map failed, count=%lu\n", count);
            DebugBreak();
        }

        // Step 3: commit to standby and clean up rescued
        for (ULONG i = 0; i < count; i++) {
            if (slots[i] == (ULONG64)-1) continue; // rescued in step 2 so slot already dealt with

            EnterCriticalSection(&standbyList_head.list_lock);

            PFN_STATE s; s.whole = pfns[i]->state.whole;
            if (s.being_written == 0) {
                // Poached during the copy
                LeaveCriticalSection(&standbyList_head.list_lock);
                return_disk_free_slots(slots[i]);
                InterlockedIncrement64(&disk_debug[5]);
                continue;
            }
            pfns[i]->state.being_written = 0;
            pfns[i]->state.list_type = 3;
            InsertTailList(&standbyList_head.entry, &pfns[i]->links);
            InterlockedIncrement64(&standbyList_head.list_count);
            LeaveCriticalSection(&standbyList_head.list_lock);
        }
        // Signal fault threads that standby has pages
        SetEvent(redoFault_event);
    }
}

DWORD WINAPI
page_fault_thread(
    PVOID parameter
) {
    thread_index = (int)(ULONG_PTR)parameter;
    unsigned i;
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
    printf("Page fault thread %i: starting virtual memory simulation workload...\n", thread_index);
    QueryPerformanceCounter(&start_time);

    BOOL page_faulted = FALSE;
    BOOL fault_resolution = TRUE;
    PULONG_PTR arbitrary_va;

    // Now perform random accesses.
    for (i = 1; i <= (100 * MB(1) / 1); i += 1) {
        InterlockedIncrement64(&va_access_count);
        page_faulted = FALSE;

        // If arbitrary VA is empty or successfully stamped, generate next arbitrary VA
        if (fault_resolution == TRUE) {
            random_number = GetNextRandom(&thread_rng);
            random_number %= virtual_address_size_in_unsigned_chunks;

            random_number &= ~0x7;
            arbitrary_va = VA_SPACE + random_number;
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
            fault_resolution = TRUE;
            PPTE pte = get_pte_from_va(arbitrary_va);
            // Change their list
            // Reset the age list
        }
        if (i % 100000 == 0) printf(". ");
    }

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

// thread 3
DWORD WINAPI
trim_thread(
    PVOID parameter
) {
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
        HANDLE waits[2] = { startTrim_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        if (zeroList_head.list_count < LOW_ZERO_PAGE_THRESHOLD) {
            SetEvent(startZero_event);
        }
        // Only trim if running low on pages that we can reclaim
        ULONG64 reclaimable = freeList_head.list_count + standbyList_head.list_count;
        if (reclaimable < LOW_FREE_PAGE_THRESHOLD) {
            ULONG64 now = GetTickCount64();
            if (now - last_age_tick >= MIN_AGE_INTERVAL_MS) {
                last_age_tick = now;
                SetEvent(startAge_event);
            }
            
            // Dynamic batch: trim enough to overshoot threshold
            ULONG64 target = LOW_FREE_PAGE_THRESHOLD + (LOW_FREE_PAGE_THRESHOLD / 2);
            ULONG64 batch = (reclaimable < target) ? (target - reclaimable) : MIN_TRIM_BATCH;
            if (batch < MIN_TRIM_BATCH) batch = MIN_TRIM_BATCH;
            if (batch > MAX_TRIM_PAGES)  batch = MAX_TRIM_PAGES;

            count = 0;
            get_unmap_candidates_and_trim(&count, MAX_TRIM_PAGES);
        
            if (count > 0 || modifiedList_head.list_count > 0) { // ZS add threshold?
                SetEvent(modifiedReady_event);  // wake up disk thread if there are trimmed pages
            }
            // Re-arm if still under pressure, regardless of whether we trimmed
            ULONG64 now_reclaimable = freeList_head.list_count + standbyList_head.list_count;
            if (now_reclaimable < LOW_FREE_PAGE_THRESHOLD && WaitForSingleObject(shutdown_event, 0) != WAIT_OBJECT_0) {
                SetEvent(startTrim_event);
            }
        }
        else {
            SetEvent(redoFault_event);
        }
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

// thread 4
DWORD WINAPI
disc_thread(
    PVOID parameter
)
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


        write_modified_list();
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

// thread 5: aging (pressure-driven)
DWORD WINAPI
age_thread(
    PVOID parameter
) {
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
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        AgeListTick();
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

    // Only waits for shutdown
    WaitForSingleObject(shutdown_event, INFINITE);

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
    PULONG_PTR batch_base = (PULONG_PTR)((char*)temp_va_base + ((SIZE_T)thread_index * WRITE_BATCH * PAGE_SIZE));

    for (;;) {
        HANDLE waits[2] = { startZero_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) break;

        for (;;) {
            ULONG_PTR frames[WRITE_BATCH];
            pfn_metadata* pfns[WRITE_BATCH];
            ULONG count = 0;

            // Get pfn off of free list
            EnterCriticalSection(&freeList_head.list_lock);
            if (IsListEmpty(&freeList_head.entry)) {
                LeaveCriticalSection(&freeList_head.list_lock);
                break;
            }
            PLIST_ENTRY e = RemoveHeadList(&freeList_head.entry);
            InterlockedDecrement64(&freeList_head.list_count);
            LeaveCriticalSection(&freeList_head.list_lock);
            pfn_metadata* pfn = get_pfn_from_PListEntry(e);

            // Map to thread's scratch slot, memset, unmap
            if (!pfn->is_zero) {
                scrub_frame(pfn);
                pfn->is_zero = 1;
            }

            // Move to zero list
            EnterCriticalSection(&zeroList_head.list_lock);
            InsertTailList(&zeroList_head.entry, &pfn->links);
            InterlockedIncrement64(&zeroList_head.list_count);
            LeaveCriticalSection(&zeroList_head.list_lock);
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
        threads[i] = CreateThread(NULL, 0, page_fault_thread, (PVOID)(ULONG_PTR)i, 0, NULL);
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
    print_age_list_histogram();
    print_statistics();
    print_mm_stats();

    SetEvent(shutdown_event);

    WaitForMultipleObjects(NUM_THREADS - NUM_FAULT_THREADS,
                           &threads[NUM_FAULT_THREADS], TRUE, INFINITE);

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
    DeleteCriticalSection(&freeList_head.list_lock);
    DeleteCriticalSection(&activeList_head.list_lock);
    DeleteCriticalSection(&modifiedList_head.list_lock);
    DeleteCriticalSection(&standbyList_head.list_lock);
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