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
// NEW ORDER: ZS change
// region (pte) lock
//      ↓
// list lock(active / modified / standby / free)
//      ↓
// pfn lock(per - page)
//      ↓
// disc lock
// Back - edges(region acquired while holding a list) MUST use TryEnter and skip on failure.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

//
// This define enables code that lets us create multiple virtual address
// mappings to a single physical page.  We only/need want this if/when we
// start using reference counts to avoid holding locks while performing
// pagefile I/Os - because otherwise disallowing this makes it easier to
// detect and fix unintended failures to unmap virtual addresses properly.
//

#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 1

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

#define PAGE_SIZE                   4096

#define KB(x)                       ((x) * 1024)
#define MB(x)                       ((x) * 1024 * 1024)
#define GB(x)                       ((x) * 1024 * 1024 * 1024)

//
// This is intentionally a power of two so we can use masking to stay
// within bounds.
//

// #define VIRTUAL_ADDRESS_SIZE        KB(16)

// #define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

//
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//

#define NUMBER_OF_PHYSICAL_PAGES   (GB(1) / PAGE_SIZE)

#define NUM_DISC_PAGES  (3 * NUMBER_OF_PHYSICAL_PAGES)

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)

#define MAX_TRIM_PAGES 32

#define LOW_FREE_PAGE_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES / 8)           // start with 1/8 of physical pool as low threshold

#define PTES_PER_LOCK 512

#define NUM_FAULT_THREADS 8
#define NUM_THREADS       12     

#define INVALID_DISC_SLOT ((1ULL << MAX_DISC_PTE_BITS) - 1)

#define AGES 8

#define PERIOD_MS 1000

#define DEBUG 1

#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

// Struct for lists
typedef struct _LIST_HEAD_ENTRY {
    LIST_ENTRY entry;
    CRITICAL_SECTION list_lock;
    ULONG64 list_count; 
} LIST_HEAD, * PLIST_HEAD;

// Locks
ULONG64 NUM_PTE_LOCKS = 0;

// PTE list heads
LIST_HEAD freeList_head;
LIST_HEAD activeList_head;
LIST_HEAD modifiedList_head;
LIST_HEAD standbyList_head;

// Events
BOOL trim_running = TRUE;
HANDLE startAge_event;
HANDLE startTrim_event;         // fault thread tells trimmer to start trimming since pool of available pages is running low

HANDLE diskReady_event;         // signals when disk has free slots
HANDLE modifiedReady_event;     // signals when trimmer has successfully trimmed pages to modified list

HANDLE redoFault_event;         // disk writer/trimmer tells fault threads that pages are available or to retry
HANDLE shutdown_event;          // shutdown all threads at end of program ZS need to implement this

unsigned va_access_count;


// Struct for our PTEs
typedef struct _VALID_PTE {
    ULONG64 valid : 1;
    ULONG64 accessed : 1;
    ULONG64 age : 3;
    ULONG64 frame_number : 40;
    ULONG64 reserved : 19;
} VALID_PTE, * PVALID_PTE;

typedef struct _TRANSITION_PTE {
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 1
    ULONG64 frame_number : 40;
    ULONG64 reserved : 22;
} TRANSITION_PTE, * PTRANSITION_PTE;

typedef struct _DISC_PTE {
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 0
    ULONG64 disc : 1; // = 1
    ULONG64 disc_index : MAX_DISC_PTE_BITS;
    ULONG64 reserved : 64 - MAX_DISC_PTE_BITS - 3;
} DISC_PTE, * PDISC_PTE;

// Same 8 bytes with different defined structures
typedef struct {
    union {
        VALID_PTE hardware;
        TRANSITION_PTE transition;
        DISC_PTE disc;
    };
} PTE, * PPTE;
PPTE page_table;

// ZS implement
typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    ULONG64 active_page_count;
    ULONG64 age_counts[8];      // track number of valid PTEs in region with a certain age
} PTE_REGION, * PPTE_REGION;

PPTE_REGION pte_regions;


PULONG_PTR VA_SPACE;
PVOID temp_va_base;
__declspec(thread) int thread_index = -1;

ULONG_PTR virtual_address_size_in_unsigned_chunks;
PULONG_PTR physical_page_numbers;


// Struct for our PFNs
typedef struct _pfn_metadata {
    LIST_ENTRY links;        // free / active / modified / standby list
    CRITICAL_SECTION lock;

    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index : MAX_DISC_PTE_BITS;

    ULONG64 list_type : 2;   // 00 free, 01 active, 10 modified, 11 standby
    ULONG64 unmap_pending : 1;
    ULONG64 being_written : 1;
    ULONG64 accessed : 1;
    ULONG64 owner_thread_id;

} pfn_metadata;

// Represents physical memory slots and used as an array of pointers mapping hardware frame number to metadata block
pfn_metadata* physical_slots = NULL;
ULONG64 max_frame_number = 0;

// Struct for disc metadata
typedef struct _DISC_METADATA {
    LIST_ENTRY list;
    ULONG64 index;
    BOOL isOccupied;
} DISC_METADATA, * PDISC_METADATA;

PDISC_METADATA disc_metadata;   // array of nodes, one per disc slot
LIST_HEAD disc_free_list;       // free slots live here
PVOID disc;
ULONG64 disc_page_count;
volatile ULONG64 disk_debug[32];

ULONG64 num_ptes;

typedef struct _THREAD_RNG_STATE {
    ULONG64 s0;
    ULONG64 s1;
    unsigned int counter;
} THREAD_RNG_STATE;


// ---- Forward declarations ----

// RNG
VOID SeedRng(THREAD_RNG_STATE* rng);
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng);

// Linked list primitives
VOID InitializeListHead(PLIST_ENTRY ListHead);
VOID InitializeList(PLIST_HEAD Head);
BOOLEAN IsListEmpty(PLIST_ENTRY ListHead);
VOID InsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
VOID InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN RemoveEntryList(PLIST_ENTRY Entry);

// Allocation helpers
PVOID zero_malloc(size_t num_bytes);

// Getters
PPTE_REGION get_pte_region(PPTE pte);
CRITICAL_SECTION* get_pte_lock(PPTE pte);
PPTE get_pte_from_va(PULONG_PTR va);
PULONG_PTR get_va_from_pte(PPTE pte);
pfn_metadata* get_pfn_from_PListEntry(PLIST_ENTRY entry);
pfn_metadata* get_pfn_from_fn(ULONG64 fn);

// Trim / free-page acquisition
VOID get_unmap_candidates_and_trim(int* batch_count, INT batch_size);
pfn_metadata* get_free_page(VOID);
pfn_metadata* get_pfn_from_standby(CRITICAL_SECTION* my_region);
pfn_metadata* get_free_pfn(CRITICAL_SECTION* my_region);

// PTE setters
VOID set_pte_valid(PPTE pte, ULONG64 frame_number, ULONG64 age);
VOID set_pte_invalid(PPTE pte);
VOID set_pte_transition(PPTE pte, ULONG64 frame_number);
VOID set_pte_disc(PPTE pte, ULONG64 disc_index);

// Validation / frame helpers
VOID validate_page_contents(PVOID page_va, const char* site);
BOOL pool_is_empty(VOID);
VOID scrub_frame(pfn_metadata* pfn);

// Disc
PVOID create_page_file(PULONG64 number_of_pages);
ULONG64 get_disk_free_slots(VOID);
VOID return_disk_free_slots(ULONG64 slot);

// Age lists
BOOLEAN PTE_WasAccessed(PPTE pte);
VOID PTE_ClearAccessedBit(PPTE pte);
VOID AgeListTick(VOID);

// Initialization
VOID init_lists(VOID);
VOID init_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);
VOID init_disc(VOID);
VOID init_global_locks(VOID);
VOID init_pte_regions(VOID);
VOID init_events(VOID);
VOID init(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);

// Diagnostics
VOID print_age_list_histogram(VOID);

// Privilege / setup
BOOL GetPrivilege(VOID);
#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
HANDLE CreateSharedMemorySection(VOID);
#endif
BOOL setup_program(VOID);

// Fault handling
BOOLEAN handle_soft_fault(PVOID arbitrary_va);
BOOL handle_hard_fault(PVOID arbitrary_va);
VOID write_modified_list(VOID);

// Thread entry points
DWORD WINAPI page_fault_thread(PVOID parameter);
DWORD WINAPI trim_thread(PVOID parameter);
DWORD WINAPI disc_thread(PVOID parameter);
DWORD WINAPI age_thread(PVOID parameter);

// Test driver
VOID full_virtual_memory_test(VOID);


VOID 
SeedRng(
    THREAD_RNG_STATE* rng
) {
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
GetNextRandom(
    THREAD_RNG_STATE* rng
) {
    ULONG64 s1 = rng->s0;
    const ULONG64 s0 = rng->s1;
    const ULONG64 result = s0 + s1;

    rng->s0 = s0;
    s1 ^= s1 << 23;
    rng->s1 = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);

    rng->counter++;
    if ((rng->counter & 0xFFFF) == 0) {
        rng->s0 ^= __rdtsc();
        if (rng->s0 == 0 && rng->s1 == 0) rng->s0 = 1;
    }

    return result;
}


// Functions for Linked Lists
VOID
InitializeListHead(
    PLIST_ENTRY ListHead
)
{
    ListHead->Flink = ListHead->Blink = ListHead;
    return;
}

VOID
InitializeList(
    PLIST_HEAD Head
) {
    InitializeListHead(&Head->entry);
    InitializeCriticalSectionAndSpinCount(&Head->list_lock, 0x00FFFFFF);
    Head->list_count = 0;
    return;
}

BOOLEAN
IsListEmpty(
    PLIST_ENTRY ListHead
)
{
    return (BOOLEAN)(ListHead->Flink == ListHead);
}

VOID
InsertHeadList(
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Flink;

    //
    // Insert a new entry at the head.
    //

    Flink = ListHead->Flink;

    Entry->Flink = Flink;
    Entry->Blink = ListHead;

    Flink->Blink = Entry;

    ListHead->Flink = Entry;

    return;
}


VOID
InsertTailList(
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;

    //
    // Insert a new entry at the tail.
    //

    Blink = ListHead->Blink;

    Entry->Flink = ListHead;
    Entry->Blink = Blink;

    Blink->Flink = Entry;

    ListHead->Blink = Entry;

    return;
}

PLIST_ENTRY
RemoveHeadList(
    PLIST_ENTRY ListHead
)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    //
    // Remove the entry currently at the head of the list.
    //

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

BOOLEAN
RemoveEntryList(
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    //
    // Remove the caller's known entry.
    //

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    //
    // Return whether list is now empty.
    //

    return (BOOLEAN)(Flink == Blink);
}

// Functions and helpers

// Malloc and memset vm to zero
PVOID
zero_malloc(
    size_t num_bytes
) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}

// Getters
// Getting pte region locks
PPTE_REGION
get_pte_region(
    PPTE pte
) {
    ULONG64 pte_index = pte - page_table;
    ASSERT(pte >= page_table);
    ASSERT(pte_index / PTES_PER_LOCK < NUM_PTE_LOCKS);
    return &pte_regions[pte_index / PTES_PER_LOCK];
}

CRITICAL_SECTION*
get_pte_lock(
    PPTE pte
) {
    return &get_pte_region(pte)->lock;
}

// VA -> PTE
PPTE 
get_pte_from_va(
    PULONG_PTR va
) {
    ULONG64 index = ((ULONG64)va - (ULONG64)VA_SPACE) / PAGE_SIZE;
    return page_table + index;
}

// PTE -> VA
PULONG_PTR
get_va_from_pte(
    PPTE pte
) {
    ULONG64 pte_index = pte - page_table;
    PULONG_PTR page_va = VA_SPACE + (pte_index * (PAGE_SIZE / sizeof(ULONG_PTR)));
    return (PULONG_PTR)((ULONG_PTR)page_va & ~(PAGE_SIZE - 1)); 
}

// PListEntry -> metadata
pfn_metadata*
get_pfn_from_PListEntry(
    PLIST_ENTRY entry
) {
    if (entry == NULL) return NULL;
    return CONTAINING_RECORD(entry, pfn_metadata, links);
}

// FrameNumber -> PFN
pfn_metadata*
get_pfn_from_fn(
    ULONG64 fn
) {
    if (fn <= max_frame_number) {
        return &physical_slots[fn];
    }
    DebugBreak();
    return NULL;
}

VOID
get_unmap_candidates_and_trim(
    int* batch_count,
    INT batch_size
) {
    PULONG_PTR unmap_vas[MAX_TRIM_PAGES] = { NULL };
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES];
    for (int age = 7; age > 1 && *batch_count < batch_size; age--) {

        for (ULONG64 i = 0; i < NUM_PTE_LOCKS && *batch_count < batch_size; i++) {
            PPTE_REGION region = &pte_regions[i];

            if (region->active_page_count == 0) continue;
            if (!TryEnterCriticalSection(&region->lock)) continue;
            if (region->age_counts[age] == 0) { // recheck under lock
                LeaveCriticalSection(&region->lock);
                continue;
            }

            ULONG64 start = i * PTES_PER_LOCK;
            ULONG64 end = min(start + PTES_PER_LOCK, num_ptes);
            for (ULONG64 j = start; j < end && *batch_count < batch_size; j++) {
                PPTE pte = page_table + j;
                PTE snap;
                *(ULONG64*)&snap = *(volatile ULONG64*)pte;
                if (snap.hardware.valid != 1) continue;
                if (snap.hardware.age != (ULONG64)age) continue;

                pfn_metadata* pfn = get_pfn_from_fn(snap.hardware.frame_number);

                EnterCriticalSection(&activeList_head.list_lock);
                EnterCriticalSection(&modifiedList_head.list_lock);
                EnterCriticalSection(&pfn->lock);

                if (pfn->being_written) {
                    LeaveCriticalSection(&pfn->lock);
                    LeaveCriticalSection(&modifiedList_head.list_lock);
                    LeaveCriticalSection(&activeList_head.list_lock);
                    continue;
                }

                // try enter critical section for the specific pte region here
                // either do this and unmap w/ lock at end of loop or continue to next
                PULONG_PTR victim_va = get_va_from_pte(pte);
                pfn->unmap_pending = TRUE;          // set this flag before dropping region
                // Set pte to transition state
                set_pte_transition(pte, pfn->frame_number);
                region->active_page_count--;
                region->age_counts[snap.hardware.age]--;

                unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)victim_va & ~(PAGE_SIZE - 1));
                unmap_pfns[*batch_count] = pfn;
                (*batch_count)++;

                // Remove from active list
                RemoveEntryList(&pfn->links);
                activeList_head.list_count--;

                pfn->list_type = 2;
                InsertTailList(&modifiedList_head.entry, &pfn->links);
                modifiedList_head.list_count++;

                LeaveCriticalSection(&pfn->lock);
                LeaveCriticalSection(&modifiedList_head.list_lock);
                LeaveCriticalSection(&activeList_head.list_lock);
            }
            LeaveCriticalSection(&region->lock);
        }
    }

    if (*batch_count > 0) {
        // Batch unmap and unmark VAs as pending to be unmapped
        if (MapUserPhysicalPagesScatter(unmap_vas, (ULONG_PTR)*batch_count, NULL)) {
            for (int i = 0; i < *batch_count; i++) {
                EnterCriticalSection(&unmap_pfns[i]->lock);
                unmap_pfns[i]->unmap_pending = FALSE;
                LeaveCriticalSection(&unmap_pfns[i]->lock);
            }
        }
        // Batch unmap failed and pages still mapped, so don't clear pending here
        else {
            DebugBreak();
        }
    }
    return;
}

// Returns free page from FreeList
pfn_metadata*
get_free_page(
    VOID
)
{
    EnterCriticalSection(&freeList_head.list_lock);
    if (IsListEmpty(&freeList_head.entry)) {
        LeaveCriticalSection(&freeList_head.list_lock);
        return NULL;
    }
    PLIST_ENTRY entry = RemoveHeadList(&freeList_head.entry);
    freeList_head.list_count--;
    LeaveCriticalSection(&freeList_head.list_lock);
    pfn_metadata* pfn = get_pfn_from_PListEntry(entry);

    // DEBUG: catch double-allocation at the source
    ULONG64 tid = GetCurrentThreadId();
    ULONG64 prev_owner = InterlockedCompareExchange64(
        (LONG64 volatile*)&pfn->owner_thread_id, tid, 0);
    if (prev_owner != 0) {
        printf("BUG: get_free_page handed out pfn %p already owned by tid %llu (I am tid %llu)\n", pfn, prev_owner, tid);
        DebugBreak();
    }
    return pfn;
}

// Rescue a free frame from standby list in disc state
pfn_metadata*
get_pfn_from_standby(
    CRITICAL_SECTION* my_region
)
{
    pfn_metadata* result = NULL;

    EnterCriticalSection(&standbyList_head.list_lock);

    // Standby entry and its pfn
    PLIST_ENTRY standby_entry = standbyList_head.entry.Flink;

    while (standby_entry != &standbyList_head.entry) {
        pfn_metadata* new_pfn = get_pfn_from_PListEntry(standby_entry);
        EnterCriticalSection(&new_pfn->lock);
        PLIST_ENTRY next = standby_entry->Flink;

        ASSERT(new_pfn->list_type == 3);

        PPTE old_pte = new_pfn->pte;

        if (old_pte != NULL) {
            CRITICAL_SECTION* old_region = get_pte_lock(old_pte);
            if (old_region == my_region) {
                // Caller holds region lock so safe to touch directly
                RemoveEntryList(&new_pfn->links);
                standbyList_head.list_count--;

                set_pte_disc(old_pte, new_pfn->disc_index);
                new_pfn->pte = NULL;
                new_pfn->disc_index = INVALID_DISC_SLOT;

                result = new_pfn;
                LeaveCriticalSection(&new_pfn->lock);
                break;
            }
            else if (TryEnterCriticalSection(old_region)) {
                // Dif region lock acquired without blocking
                // respects current region bc not waiting on region lock with list lock
                RemoveEntryList(&new_pfn->links);
                standbyList_head.list_count--;

                set_pte_disc(old_pte, new_pfn->disc_index);
                new_pfn->pte = NULL;
                new_pfn->disc_index = INVALID_DISC_SLOT;

                LeaveCriticalSection(old_region);

                result = new_pfn;
                LeaveCriticalSection(&new_pfn->lock);
                break;
            }
        }
        // If unable to grab region lock, try next frame on standby list
        else {
            DebugBreak();
        }
        LeaveCriticalSection(&new_pfn->lock);
        standby_entry = next;
    }

    // Claim under lock so no other path can grab the same frame
    if (result != NULL) {
        result->list_type = 0;   // no longer standby --> in-transit to active
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


pfn_metadata*
get_free_pfn(
    CRITICAL_SECTION* my_region
) {
    pfn_metadata* new_pfn = get_free_page();
    if (new_pfn != NULL) return new_pfn;

    new_pfn = get_pfn_from_standby(my_region);
    if (new_pfn != NULL) return new_pfn;

    return NULL; // caller will signal trimmer and wait
}

// Setters

VOID
set_pte_valid(
    PPTE pte,
    ULONG64 frame_number,
    ULONG64 age
)
{
    ASSERT(age >= 1 && age <= 7);
    PTE new_pte;
    new_pte.hardware.valid = 1;
    new_pte.hardware.accessed = 1; // freshly faulted-in page counts as accessed but set 0 to start as cold
    new_pte.hardware.age = age;
    new_pte.hardware.frame_number = frame_number;
    new_pte.hardware.reserved = 0;
    *(volatile ULONG64*)pte = *(ULONG64*)&new_pte;
}

VOID
set_pte_invalid(
    PPTE pte
)
{
    *(volatile ULONG64*)pte = 0;
}

VOID
set_pte_transition(
    PPTE pte,
    ULONG64 frame_number
)
{
    PTE new_pte;
    new_pte.transition.valid = 0;
    new_pte.transition.transition = 1;
    new_pte.transition.frame_number = frame_number;
    new_pte.transition.reserved = 0;
    *(volatile ULONG64*)pte = *(ULONG64*)&new_pte;
}

VOID
set_pte_disc(
    PPTE pte,
    ULONG64 disc_index
)
{
    PTE new_pte;
    new_pte.disc.valid = 0;
    new_pte.disc.transition = 0;
    new_pte.disc.disc = 1;
    new_pte.disc.disc_index = disc_index;
    new_pte.disc.reserved = 0;
    *(volatile ULONG64*)pte = *(ULONG64*)&new_pte;
}

// Call after a frame is mapped+populated at its real VA with region and pfn locks
// Scans the whole page so every non-zero 8-byte slot equals its own VA
VOID
validate_page_contents(
    PVOID page_va,
    const char* site
) {
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

BOOL
pool_is_empty(
    VOID
) {
    return (freeList_head.list_count == 0 && standbyList_head.list_count == 0);
}

// Zero a frame's physical contents via this thread's scratch VA
// Guarantees a frame placed on the free list holds no live data
// Caller must NOT hold the frame mapped at any other VA
VOID
scrub_frame(
    pfn_metadata* pfn
) {
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

// Disc stuff
// Create fake disk
PVOID
create_page_file(
    PULONG64 number_of_pages
) {
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
get_disk_free_slots(
    VOID
)
{
    disk_debug[0]++;
    EnterCriticalSection(&disc_free_list.list_lock);
    if (IsListEmpty(&disc_free_list.entry)) {
        LeaveCriticalSection(&disc_free_list.list_lock);
        return (ULONG64)-1;
    }
    PLIST_ENTRY entry = RemoveHeadList(&disc_free_list.entry);
    disc_free_list.list_count--;
    disk_debug[1]++;
    LeaveCriticalSection(&disc_free_list.list_lock);

    PDISC_METADATA metadata = CONTAINING_RECORD(entry, DISC_METADATA, list);
    ASSERT(metadata->isOccupied == FALSE);
    metadata->isOccupied = TRUE;
    return metadata->index;
}

// Push back to stack and mark metadata as free
VOID
return_disk_free_slots(
    ULONG64 slot
) {
    ASSERT(slot < disc_page_count);
    PDISC_METADATA meta = &disc_metadata[slot];

    EnterCriticalSection(&disc_free_list.list_lock);

    ASSERT(meta->isOccupied == TRUE);
    meta->isOccupied = FALSE;
    InsertTailList(&disc_free_list.entry, &meta->list);
    disc_free_list.list_count++;
    // Set event here when there are free disk slots so not excessively waking s that thread
    SetEvent(diskReady_event);
    LeaveCriticalSection(&disc_free_list.list_lock);
}

// Age stuff

BOOLEAN
PTE_WasAccessed(
    PPTE pte
) {
    return (BOOLEAN)pte->hardware.accessed;
}

VOID
PTE_ClearAccessedBit(
    PPTE pte
) {
    pte->hardware.accessed = 0;
    return;
}

ULONG64 tick_call = 0; 
VOID
AgeListTick(
    VOID
) {
    tick_call++;
    // Walk through each pte region
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        PPTE_REGION region = &pte_regions[i];

        EnterCriticalSection(&region->lock);
        if (region->active_page_count == 0) {
            LeaveCriticalSection(&region->lock);
            continue;
        }

        ULONG64 start = i * PTES_PER_LOCK;
        ULONG64 end = min(start + PTES_PER_LOCK, num_ptes);
        // Walk through each pte
        for (ULONG64 j = start; j < end; j++) {
            PPTE pte = page_table + j;

            PTE snap;
            *(ULONG64*)&snap = *(volatile ULONG64*)pte;
            if (snap.hardware.valid != 1) continue; // age only valid PTEss

            if (snap.hardware.accessed) {
                snap.hardware.accessed = 0;         // touched and reset but keep age
            }
            else if (snap.hardware.age < 7) {
                region->age_counts[snap.hardware.age]--;
                snap.hardware.age++;                // untouched and age up
                region->age_counts[snap.hardware.age]++;
            }
            else {
                continue;
            }
            *(volatile ULONG64*)pte = *(ULONG64*)&snap;
        }
        LeaveCriticalSection(&region->lock);
    }
}

// Commit only the page(s) of the reserved PFN table that hold this frame's slot.
// Idempotent: MEM_COMMIT on already-committed memory succeeds and is cheap.
VOID
ensure_metadata_slot_is_committed(
    ULONG64 frame
) {
    PVOID slot_addr = (PVOID)&physical_slots[frame];

    // A slot can straddle a page boundary, so commit the whole span it covers.
    PVOID start = (PVOID)((ULONG_PTR)slot_addr & ~((ULONG_PTR)PAGE_SIZE - 1));
    PVOID last = (PVOID)(((ULONG_PTR)slot_addr + sizeof(pfn_metadata) - 1)
        & ~((ULONG_PTR)PAGE_SIZE - 1));
    SIZE_T span = (ULONG_PTR)last - (ULONG_PTR)start + PAGE_SIZE;

    if (VirtualAlloc(start, span, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        printf("ensure_metadata_slot_is_committed: commit failed for frame %llu, error %lu\n",
            frame, GetLastError());
        DebugBreak();
    }
}

// Initializations
// Initialize pte lists
VOID
init_lists(
    VOID
) {
    InitializeList(&freeList_head);
    InitializeList(&activeList_head);
    InitializeList(&modifiedList_head);
    InitializeList(&standbyList_head);
}

// Initialize pfn metadata sparse array
VOID
init_pfn_metadata(
    ULONG_PTR physical_page_count,
    PULONG_PTR physical_page_numbers
) {
    // Find max existing frame number
    for (int i = 0; i < physical_page_count; i++) {
        max_frame_number = max(physical_page_numbers[i], max_frame_number);
    }

    // Reserve max_frame_number+1 amount of slots so that frame number can be index
    ULONG_PTR table_reserve_size = (max_frame_number + 1) * sizeof(pfn_metadata);
    physical_slots = VirtualAlloc(
        NULL,
        table_reserve_size,
        MEM_RESERVE,
        PAGE_READWRITE
    );
    if (physical_slots == NULL) {
        printf("init_pfn_metadata: failed to reserve and commit physical_slots. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Setup pfn metadata in physical_slots and add to free list
    for (ULONG_PTR i = 0; i < physical_page_count; i++) {
        ULONG64 frame = physical_page_numbers[i];
        
        ensure_metadata_slot_is_committed(frame);

        physical_slots[frame].frame_number = frame;
        physical_slots[frame].pte = NULL;
        physical_slots[frame].disc_index = INVALID_DISC_SLOT;
        physical_slots[frame].list_type = 0;
        physical_slots[frame].unmap_pending = 0;
        physical_slots[frame].being_written = 0;
        physical_slots[frame].accessed = 0;
        physical_slots[frame].owner_thread_id = 0;

        InitializeListHead(&physical_slots[frame].links);
        InitializeCriticalSectionAndSpinCount(&physical_slots[frame].lock, 0x00FFFFFF);
        InsertTailList(&freeList_head.entry, &physical_slots[frame].links);
        freeList_head.list_count++;
    }
    // Commit only the used parts of pfn_metadata sparse array
    // Report actual commit footprint
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR committed = 0;
    PCHAR addr = (PCHAR)physical_slots;
    PCHAR end = addr + table_reserve_size;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
        addr += mbi.RegionSize;
    }
    printf("PFN table: committed %llu MB of %llu MB reserved (%llu frames)\n",
        (ULONG64)committed / MB(1),
        (ULONG64)table_reserve_size / MB(1),
        (ULONG64)physical_page_count);
}

// Initialize disc
VOID
init_disc(
    VOID
)
{
    disc_page_count = NUM_DISC_PAGES;
    disc = create_page_file(&disc_page_count);

    InitializeList(&disc_free_list);   // sets up list_lock + count

    // Push every slot index onto stack
    for (ULONG64 i = 0; i < disc_page_count; i++) {
        disc_metadata[i].index = i;
        disc_metadata[i].isOccupied = FALSE;
        InsertTailList(&disc_free_list.entry, &disc_metadata[i].list);
        disc_free_list.list_count++;
    }
}

// Initialize global locks
VOID
init_global_locks(
    VOID
) {
    // None anymore
}

// Initialize pte locks
VOID
init_pte_regions(
    VOID
) {
    pte_regions = malloc(NUM_PTE_LOCKS * sizeof(PTE_REGION));
    if (pte_regions == NULL) { 
        DebugBreak(); 
        return; 
    }
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        InitializeCriticalSectionAndSpinCount(&pte_regions[i].lock, 0x00FFFFFF);
        pte_regions[i].active_page_count = 0;
        for (int age = 0; age < 8; age++) {
            pte_regions[i].age_counts[age] = 0;
        }
    }
}

// Initialize events
VOID
init_events(
    VOID
) {
    // Auto-reset events
    startAge_event = CreateEvent(NULL, FALSE, FALSE, NULL);         
    startTrim_event = CreateEvent(NULL, FALSE, FALSE, NULL);     
    diskReady_event = CreateEvent(NULL, FALSE, FALSE, NULL);     
    modifiedReady_event = CreateEvent(NULL, FALSE, FALSE, NULL);  
    // Manual-reset events
    redoFault_event = CreateEvent(NULL, TRUE, FALSE, NULL);      
    shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL); // ZS implement event to shutdown all threads
}

VOID
init(
    ULONG_PTR physical_page_count,
    PULONG_PTR physical_page_numbers
) {
    init_lists();
    init_pfn_metadata(physical_page_count, physical_page_numbers);
    init_disc();
    init_events();
}

VOID
print_age_list_histogram(
    VOID
)
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

BOOL
GetPrivilege (
    VOID
)
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    //
    // This is Windows-specific code to acquire a privilege.
    // Understanding each line of it is not so important for
    // our efforts.
    //

    HANDLE hProcess;
    HANDLE Token;
    BOOL Result;

    //
    // Open the token.
    //

    hProcess = GetCurrentProcess ();

    Result = OpenProcessToken (hProcess,
                               TOKEN_ADJUST_PRIVILEGES,
                               &Token);

    if (Result == FALSE) {
        printf ("Cannot open process token.\n");
        return FALSE;
    }

    //
    // Enable the privilege. 
    //

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    //
    // Get the LUID.
    //

    Result = LookupPrivilegeValue (NULL,
                                   SE_LOCK_MEMORY_NAME,
                                   &(Info.Privilege[0].Luid));

    if (Result == FALSE) {
        printf ("Cannot get privilege\n");
        return FALSE;
    }

    //
    // Adjust the privilege.
    //

    Result = AdjustTokenPrivileges (Token,
                                    FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0,
                                    NULL,
                                    NULL);

    //
    // Check the result.
    //

    if (Result == FALSE) {
        printf ("Cannot adjust token privileges %u\n", GetLastError ());
        return FALSE;
    } 

    if (GetLastError () != ERROR_SUCCESS) {
        printf ("Cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy\n");
        return FALSE;
    }

    CloseHandle (Token);

    return TRUE;
}

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

HANDLE
CreateSharedMemorySection (
    VOID
    )
{
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Create an AWE section.  Later we deposit pages into it and/or
    // return them.
    //

    parameter.Type = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2 (INVALID_HANDLE_VALUE,
                                  NULL,
                                  SECTION_MAP_READ | SECTION_MAP_WRITE,
                                  PAGE_READWRITE,
                                  SEC_RESERVE,
                                  0,
                                  NULL,
                                  &parameter,
                                  1);

    return section;
}

#endif

BOOL
setup_program(
    VOID
)
{
    HANDLE physical_page_handle;

    //
    // Allocate the physical pages that we will be managing.
    //
    // First acquire privilege to do this since physical page control
    // is typically something the operating system reserves the sole
    // right to do.
    //

    BOOL privilege = GetPrivilege();

    if (privilege == FALSE) {
        printf("full_virtual_memory_test : could not get privilege\n");
        return FALSE;
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    physical_page_handle = CreateSharedMemorySection();

    if (physical_page_handle == NULL) {
        printf("CreateFileMapping2 failed, error %#x\n", GetLastError());
        return FALSE;
    }

#else

    physical_page_handle = GetCurrentProcess();

#endif
    init_global_locks();

    ULONG_PTR physical_page_count = NUMBER_OF_PHYSICAL_PAGES;
    physical_page_numbers = malloc(physical_page_count * sizeof(ULONG_PTR));


    if (physical_page_numbers == NULL) {
        printf("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return FALSE;
    }

    // Put PFNs in array so we can keep trac of which pages can be used and specifies the address
    BOOL allocated = AllocateUserPhysicalPages(physical_page_handle,
        &physical_page_count,
        physical_page_numbers);

    if (allocated == FALSE) {
        printf("full_virtual_memory_test : could not allocate physical pages\n");
        return FALSE;
    }

    // Create array to map PFNs in physical page count to their assigned VAs
    // track which VA each physical page is mapped to using second array (index, VA base + i*4k, or some other way to regenerate VA)
    // PULONG_PTR physical_page_to_virtual = malloc(physical_page_count * sizeof (ULONG_PTR));
    PULONG64 physical_page_to_virtual = malloc(physical_page_count * sizeof(ULONG64));


    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        printf("full_virtual_emory_test : allocated only %llu pages out of %u pages requested\n",
            physical_page_count,
            NUMBER_OF_PHYSICAL_PAGES);
    }

    init(physical_page_count, physical_page_numbers);


    // Set virtual address size based on physical and virtual page counts
    ULONG_PTR virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    // Round down to a PAGE_SIZE boundary
    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    // Set up page table for PTEs
    num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(num_ptes * sizeof(PTE));
    NUM_PTE_LOCKS = (num_ptes / 512) + 1;

    // Init PTE locks
    init_pte_regions();

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Allocate a MEM_PHYSICAL region that is "connected" to the AWE section
    // created above.
    //

    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;
    // mem_commit: want to use virtual memory later but cannot access it
    // mem_reserve: reserve virtual memory without allocating physical pages
    // mem_physical: map physical page into virtual address space
    VA_SPACE = VirtualAlloc2(NULL,
        NULL,
        virtual_address_size,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE,
        &parameter,
        1);

    // Reserve space for temp_va
    temp_va_base = VirtualAlloc2(NULL,
        NULL,
        (SIZE_T)PAGE_SIZE * NUM_THREADS,   // one scratch page per thread
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE,
        &parameter,                         // same parameter as VA_SPACE
        1);
    if (temp_va_base == NULL) {
        printf("setup_program: could not reserve temp_va_base %x\n", GetLastError());
        return FALSE;
    }

#else
    VA_SPACE = VirtualAlloc(NULL,
        virtual_address_size,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE);

#endif

    if (VA_SPACE == NULL) {

        printf("full_virtual_memory_test : could not reserve memory %x\n",
            GetLastError());

        return FALSE;
    }
    return TRUE;
}

BOOLEAN
handle_soft_fault(
    PVOID arbitrary_va
) {
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte); 
    CRITICAL_SECTION* region = get_pte_lock(pte);
    EnterCriticalSection(region);

    // Re-check under lock since state may have changed before we got here
    if (pte->hardware.valid == 1) {
        LeaveCriticalSection(region);
        return TRUE; // already resolved by someone else
    }
    if (pte->transition.transition != 1) {
        // Not a transition PTE anymore (ex. became disc) so not ours to handle
        LeaveCriticalSection(region);
        return FALSE;
    }

    ULONG64 frame = pte->transition.frame_number;
    pfn_metadata* pfn = get_pfn_from_fn(frame);

    EnterCriticalSection(&pfn->lock);
    if (pfn->unmap_pending == 1) {
        LeaveCriticalSection(&pfn->lock);
        LeaveCriticalSection(region);
        return FALSE;   // not resolved since VA is being unmapped and caller retries the access
    }
    BOOL poached = (pfn->being_written == 1);
    ULONG64 lt = pfn->list_type;

    // Flag to let modified disc writer that we poached pfn
    if (poached) pfn->being_written = 0;
    LeaveCriticalSection(&pfn->lock);

    // Normal rescue from disc
    if (!poached) {
        // Specify standby or modified list
        PLIST_HEAD list_head = (lt == 3) ? &standbyList_head : &modifiedList_head;
        EnterCriticalSection(&list_head->list_lock);       
        EnterCriticalSection(&pfn->lock);

        if (pfn->list_type != lt || pfn->being_written) {
            LeaveCriticalSection(&pfn->lock);
            LeaveCriticalSection(&list_head->list_lock);
            LeaveCriticalSection(region);
            return FALSE;
        }
        if (lt == 3) {
            // Set invalid disk slot
            return_disk_free_slots(pfn->disc_index); // clears disc_slot_owner
            disk_debug[2]++;
        }
        RemoveEntryList(&pfn->links);
        ASSERT(list_head->list_count != 0);
        list_head->list_count--;
        LeaveCriticalSection(&pfn->lock);
        LeaveCriticalSection(&list_head->list_lock);
    }

    PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
    if (MapUserPhysicalPages(page_aligned_va, 1, &frame) == FALSE) {
        printf("handle_page_fault: rescue remap failed\n");
        DebugBreak();
    }
    validate_page_contents(page_aligned_va, "1562");

    // Reacquire lock to finish putting page back on active list
    EnterCriticalSection(&activeList_head.list_lock);
    EnterCriticalSection(&pfn->lock);

    pfn->list_type = 1;
    pfn->pte = pte;
    InsertTailList(&activeList_head.entry, &pfn->links);

    set_pte_valid(pte, frame, 1);
    activeList_head.list_count++;
    region_struct->active_page_count++; 
    region_struct->age_counts[1]++;
    LeaveCriticalSection(&activeList_head.list_lock);

    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(region);
    return TRUE;
}

BOOL
handle_hard_fault(
    PVOID arbitrary_va
) {
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte);
    CRITICAL_SECTION* region = get_pte_lock(pte);

    // STEP 1: get a free pfn (free > standby > trim)
    pfn_metadata* new_pfn = NULL;
    int attempts = 0;

    while (new_pfn == NULL) {
        EnterCriticalSection(region);

        // Already resolved by someone else
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            return TRUE;
        }

        // Soft fault so someone else's rescue is in progress/going to start
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            BOOLEAN resolved = handle_soft_fault(arbitrary_va);
            if (resolved) return TRUE;
            continue;
        }

        // Genuine hard fault path: not valid, not transition -> try to get a page
        LeaveCriticalSection(region);

        new_pfn = get_free_pfn(NULL);
        if (new_pfn != NULL) break;

        // Not resolved and still in progress so wait and recheck
        // Pool is empty so ask trimmer for pages 
        SetEvent(startTrim_event);

        // Synchronize redoFault event with the SetEvent in modified writer
        EnterCriticalSection(&standbyList_head.list_lock);
        BOOL no_pages = pool_is_empty();
        if (no_pages) ResetEvent(redoFault_event);
        LeaveCriticalSection(&standbyList_head.list_lock);
        if (!no_pages) continue;
        WaitForSingleObject(redoFault_event, INFINITE);

        if (++attempts > 1000) {
            printf("handle_hard_fault: pool exhausted after %d retries\n", attempts);
            DebugBreak();
            return FALSE;
        }
    }

    // STEP 2: if we got a page, map physical page to faulting VA
    EnterCriticalSection(region);
    scrub_frame(new_pfn);

    if (new_pfn != NULL) {
        // PTE may have been resolved while pte_lock was dropped or on first-try path
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            // Another thread has restored this page, so return pfn and don't map/copy
            EnterCriticalSection(&freeList_head.list_lock);
            new_pfn->list_type = 0;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0; 

            InsertHeadList(&freeList_head.entry, &new_pfn->links);
            freeList_head.list_count++;
            LeaveCriticalSection(&freeList_head.list_lock);
            return TRUE;
        }
        if (pte->transition.transition == 1) {
            // No longer a disc PTE, resolved by another path
            LeaveCriticalSection(region);
            EnterCriticalSection(&freeList_head.list_lock);
            new_pfn->list_type = 0;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;

            InsertHeadList(&freeList_head.entry, &new_pfn->links);
            freeList_head.list_count++;
            LeaveCriticalSection(&freeList_head.list_lock);

            return FALSE; // retry from top
        }
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;

        PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
        PULONG_PTR temp_va = (PULONG_PTR)((char*)temp_va_base + (thread_index * PAGE_SIZE));        
        
        // STEP 3: restore from disk if needed
        if (from_disc) {
            // Map the frame to a per-thread scratch VA nobody else knows about.
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
                printf("hard fault: slot %llu holds data for VA %p, expected %p\n",
                    old_disc_slot, (PVOID)first, page_aligned_va);
                DebugBreak();
            }

            // Unmap from temp before remapping elsewhere — a frame can't be mapped at two VAs simultaneously.
            if (MapUserPhysicalPages(temp_va, 1, NULL) == FALSE) {
                printf("could not unmap frame %llX from temp_va\n", new_pfn->frame_number);
                LeaveCriticalSection(region);
                DebugBreak();
                return FALSE;
            }

            return_disk_free_slots(old_disc_slot); // Free up slot (clears disc_slot_owner)
            disk_debug[3]++;
        }

        // Now expose the (already-populated, or zero for fresh) frame at the real VA.
        if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
            printf("could not map VA %p to page %llX\n", arbitrary_va, new_pfn->frame_number);
            LeaveCriticalSection(region);
            DebugBreak();
            return FALSE;
        }
        validate_page_contents(page_aligned_va, "1695");


        // STEP 4: otherwise clear it
        if (!from_disc) {
            memset(page_aligned_va, 0, PAGE_SIZE);
        }

        // STEP 5: set to active state and update pte and pfn metadata
        EnterCriticalSection(&activeList_head.list_lock);
        new_pfn->being_written = 0;
        new_pfn->list_type = 1;
        new_pfn->pte = pte;
        new_pfn->owner_thread_id = 0;  

        InsertTailList(&activeList_head.entry, &new_pfn->links);

        set_pte_valid(pte, new_pfn->frame_number, 1);
        activeList_head.list_count++;
        region_struct->active_page_count++;
        region_struct->age_counts[1]++;

        LeaveCriticalSection(&activeList_head.list_lock);
        LeaveCriticalSection(region);
    }
    return TRUE;
}

// Move modified list to disk and push to standby
VOID
write_modified_list(
    VOID
) {
    // ZS squeeze right after this check, then 20 ms wait to recheck but should avoid that
    while (!IsListEmpty(&modifiedList_head.entry)) {
        EnterCriticalSection(&modifiedList_head.list_lock);
        if (IsListEmpty(&modifiedList_head.entry)) {        // Soft faulter pulled all modified entries
            LeaveCriticalSection(&modifiedList_head.list_lock);
            break;
        }

        // Grab page off modified list and mark as in transition
        PLIST_ENTRY entry = RemoveHeadList(&modifiedList_head.entry);
        modifiedList_head.list_count--;
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);
        EnterCriticalSection(&pfn->lock);

        if (pfn->unmap_pending) {
            InsertHeadList(&modifiedList_head, entry);
            modifiedList_head.list_count++;
            LeaveCriticalSection(&pfn->lock);
            LeaveCriticalSection(&modifiedList_head.list_lock);
            break;
        }

        ASSERT(pfn->being_written == 0);
        ASSERT(pfn->list_type == 2);
        ASSERT(pfn->pte != NULL);

        pfn->being_written = 1;

        LeaveCriticalSection(&modifiedList_head.list_lock);
        LeaveCriticalSection(&pfn->lock);


        // No locks are held here, need to recheck logic

        // Get disk info outside of the lock
        ULONG64 slot = get_disk_free_slots();
        if (slot == (ULONG64)-1) {
            // If the disk is full undo transition and put it back on modified
            EnterCriticalSection(&pfn->lock);
            EnterCriticalSection(&modifiedList_head.list_lock);
            // Pfn has not been rescued yet
            if (pfn->being_written == 1) {
                pfn->being_written = 0;
                InsertHeadList(&modifiedList_head.entry, &pfn->links);
                modifiedList_head.list_count++;
            }
            LeaveCriticalSection(&modifiedList_head.list_lock);
            LeaveCriticalSection(&pfn->lock);
            WaitForSingleObject(diskReady_event, INFINITE); // moved from disc thread
            continue;
        }

        // Reacquire lock to update metadata and move to standby
        EnterCriticalSection(&pfn->lock);
        if (pfn->being_written == 0) {
            // Pfn was rescued during memcpy so disc entry is stale and throw it away
            LeaveCriticalSection(&pfn->lock);
            return_disk_free_slots(slot);
            disk_debug[4]++;
            continue;
        }
        pfn->disc_index = slot;
        LeaveCriticalSection(&pfn->lock);

        // Copy via temp_va ZS batch?
        PULONG_PTR temp_va = (PULONG_PTR)((char*)temp_va_base + ((SIZE_T)thread_index * PAGE_SIZE));
        MapUserPhysicalPages(temp_va, 1, &pfn->frame_number);
        memcpy((BYTE*)disc + slot * PAGE_SIZE, temp_va, PAGE_SIZE);
        MapUserPhysicalPages(temp_va, 1, NULL);

        EnterCriticalSection(&pfn->lock);
        if (pfn->being_written == 0) {
            LeaveCriticalSection(&pfn->lock);
            return_disk_free_slots(slot);
            disk_debug[5]++;
            continue;
        }

        pfn->being_written = 0;

        // Move to standby list
        pfn->list_type = 3;
        EnterCriticalSection(&standbyList_head.list_lock);
        InsertTailList(&standbyList_head.entry, &pfn->links);
        standbyList_head.list_count++;
        LeaveCriticalSection(&standbyList_head.list_lock);

        LeaveCriticalSection(&pfn->lock);
    }

    // Signal fault threads that standby has pages
    SetEvent(redoFault_event);
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
    printf("Page fault thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    BOOL page_faulted = FALSE;
    BOOL fault_resolution = TRUE;
    PULONG_PTR arbitrary_va;

    //
    // Now perform random accesses.
    // 
    //
    for (i = 1; i <= (1 * MB(1) / 1); i += 1) {
        va_access_count++;
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

            // Snapshot the PTE once — a concurrent writer can change it between reads
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
    printf("Trim thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    while (trim_running) {
        WaitForSingleObject(startTrim_event, INFINITE);
        if (!trim_running) break;

        // Only trim if running low on pages that we can reclaim
        ULONG64 reclaimable = freeList_head.list_count + standbyList_head.list_count;
        if (reclaimable < LOW_FREE_PAGE_THRESHOLD) {
            SetEvent(startAge_event);
            count = 0;
            // ZS add modified list lock here?
            if (modifiedList_head.list_count == 0) {
                // Only get candidates (disk thread will do the work)
                get_unmap_candidates_and_trim(&count, MAX_TRIM_PAGES);
            }
            if (count > 0 || modifiedList_head.list_count > 0) { // ZS add threshold?
                SetEvent(modifiedReady_event);  // Wake up disk thread if there are trimmed pages
            }
            if (count == 0 && modifiedList_head.list_count == 0) {
                SetEvent(startAge_event);
                SetEvent(startTrim_event);
            }
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
    printf("Disc thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);
    
    while (TRUE) {
        // Sleep until trim thread signals there's work
        WaitForSingleObject(modifiedReady_event, INFINITE);

        if (!trim_running) break;
        write_modified_list();
    }

    // Drain anything remaining before exit
    write_modified_list();

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Disk thread: WORKLOAD COMPLETE\n");
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
    while (trim_running) {
        WaitForSingleObject(startAge_event, INFINITE);
        if (!trim_running) break;
        AgeListTick();
    }
    return 0;
}

// thread type 5: periodic
DWORD WINAPI
periodic_thread(
    PVOID parameter
) {
    thread_index = (int)(ULONG_PTR)parameter;
    while (trim_running) {
        DWORD r = WaitForSingleObject(shutdown_event, PERIOD_MS);
        if (r == WAIT_OBJECT_0) break;   // shutdown signalled
        if (!trim_running) break;
    }
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
    threads[NUM_FAULT_THREADS+3] = CreateThread(NULL, 0, periodic_thread, (PVOID)(ULONG_PTR)10, 0, NULL);

    if (threads[0] == NULL || threads[1] == NULL || threads[2] == NULL) {
        printf("Failed to create threads. Error: %lu\n", GetLastError());
        return;
    }
    // Wait for fault threads to finish
    WaitForMultipleObjects(NUM_FAULT_THREADS, threads, TRUE, INFINITE);
    print_age_list_histogram();

    SetEvent(startAge_event);
    SetEvent(startTrim_event);
    SetEvent(diskReady_event);
    SetEvent(modifiedReady_event);
    SetEvent(redoFault_event);
    SetEvent(shutdown_event);

    trim_running = FALSE;
    WaitForMultipleObjects(NUM_THREADS-NUM_FAULT_THREADS, threads, TRUE, INFINITE);

    // Close all handles
    for (int i = 0; i < NUM_THREADS; i++) {
        CloseHandle(threads[i]);
    }
    CloseHandle(startAge_event);
    CloseHandle(startTrim_event);
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
    DeleteCriticalSection(&disc_free_list.list_lock);
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

