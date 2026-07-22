// vm.h : Shared declarations for the ZoeVM memory manager.
//
// This header lets vm.c and init.c share the same configuration constants,
// type definitions, global state, and function prototypes. The globals are
// DEFINED in vm.c; here they are only declared `extern`.

#pragma once

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
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//

#define NUMBER_OF_PHYSICAL_PAGES   (GB(1) / PAGE_SIZE)

#define NUM_DISC_PAGES  (NUMBER_OF_PHYSICAL_PAGES)

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)

#define INVALID_DISC_SLOT ((1ULL << MAX_DISC_PTE_BITS) - 1)

#define PTES_PER_LOCK 512

#define NUM_FAULT_THREADS 8
#define NUM_THREADS       13

#define MAX_TRIM_PAGES 512
#define MIN_TRIM_BATCH 32
#define LOW_ZERO_PAGE_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES / 256)
#define LOW_FREE_PAGE_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES / 64)

#define AGES 8

#define PERIOD_MS 1000
#define AGE_SLICE_DIVISOR 8

#define MIN_AGE_INTERVAL_MS 10

#define WRITE_BATCH 64

#define DEBUG 1

#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

// ---- Types ----

// Struct for lists
typedef struct _LIST_HEAD_ENTRY {
    LIST_ENTRY entry;
    CRITICAL_SECTION list_lock;
    ULONG64 list_count;
} LIST_HEAD, * PLIST_HEAD;

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
        ULONG64 entire_contents;
    };
} PTE, * PPTE;

// Struct for PTE regions
typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    ULONG64 active_page_count;
    ULONG64 age_counts[AGES];   // track number of valid PTEs in region with a certain age
} PTE_REGION, * PPTE_REGION;

// Struct for PFN states
typedef union _PFN_STATE {
    struct {
        ULONG64 list_type : 2;   // 00 free, 01 active, 10 modified, 11 standby
        ULONG64 being_written : 1;
        ULONG64 accessed : 1;
    };
    ULONG64 whole;
} PFN_STATE;

// Struct for our PFNs
typedef struct _pfn_metadata {
    LIST_ENTRY links;           // free / active / modified / standby list
    CRITICAL_SECTION lock;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index : MAX_DISC_PTE_BITS;
    // INVARIANT: state may only be read-decided-written while holding the list lock
    // for the list named by state.list_type (the source list) AND the list lock for
    // the destination list, held across the whole read-decide-write. Plain accesses
    // only -- the locks are the mechanism, there is no lock-free path to this field.
    PFN_STATE state;
    ULONG64 owner_thread_id;
    BOOLEAN is_zero;
} pfn_metadata;

// Struct for disc metadata
typedef struct _DISC_METADATA {
    LIST_ENTRY list;
    ULONG64 index;
    BOOL isOccupied;
} DISC_METADATA, * PDISC_METADATA;

// Per-category counter block
typedef struct _MM_STAT {
    volatile LONG64 calls;
    volatile LONG64 ticks;      // raw QPC ticks
    volatile LONG64 ticks_sub;  // secondary timed region (e.g. memcpy)
    volatile LONG64 pages;
} MM_STAT;

typedef struct _THREAD_RNG_STATE {
    ULONG64 s0;
    ULONG64 s1;
    unsigned int counter;
} THREAD_RNG_STATE;

// ---- Global state (defined in vm.c) ----

// Locks
extern ULONG64 NUM_PTE_LOCKS;

// PTE list heads
extern LIST_HEAD freeList_head;
extern LIST_HEAD activeList_head;
extern LIST_HEAD modifiedList_head;
extern LIST_HEAD standbyList_head;
extern LIST_HEAD zeroList_head;

// Events
extern HANDLE startAge_event;
extern HANDLE startTrim_event;      // fault thread tells trimmer to start trimming since pool of available pages is running low
extern HANDLE startZero_event;      // signal zeroing thread that free list has dirty pages

extern HANDLE diskReady_event;      // signals when disk has free slots
extern HANDLE modifiedReady_event;  // signals when trimmer has successfully trimmed pages to modified list

extern HANDLE redoFault_event;      // disk writer/trimmer tells fault threads that pages are available or to retry
extern HANDLE shutdown_event;       // manual-reset: signals all worker threads to exit at end of program

// Page table
extern PPTE page_table;
extern PPTE_REGION pte_regions;
extern ULONG64 num_ptes;

// Represents physical memory slots and used as an array of pointers mapping hardware frame number to metadata block
extern pfn_metadata* physical_slots;
extern ULONG64 max_frame_number;

// Disc
extern PDISC_METADATA disc_metadata;      // array of nodes, one per disc slot
extern ULONG64* disc_free_stack;          // array of free slot indices
extern volatile LONG64 disc_stack_top;    // index of next push slot == current count of free slots
extern CRITICAL_SECTION disc_stack_lock;  // keep the lock for now (bitmap/lock-free is a later step)
extern PVOID disc;
extern ULONG64 disc_page_count;

// Counters
extern volatile LONG64 va_access_count;
extern volatile LONG64 tick_call;
extern volatile LONG64 disk_debug[32];
extern volatile LONG64 hard_fault_count;
extern volatile LONG64 soft_fault_count;

// Per-category counter block
static MM_STAT g_trim_stat;
static MM_STAT g_write_stat;
static LARGE_INTEGER g_qpc_freq;   // set once via QueryPerformanceFrequency

// Other globals
extern PULONG_PTR VA_SPACE;
extern PVOID temp_va_base;
extern ULONG_PTR virtual_address_size_in_unsigned_chunks;
extern PULONG_PTR physical_page_numbers;

extern __declspec(thread) int thread_index;

extern ULONG64 last_age_tick;
extern ULONG64 age_cursor;      // only age_thread touches it for now

// ---- Function prototypes ----

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
pfn_metadata* get_pfn_from_free(VOID);
pfn_metadata* get_pfn_from_standby(VOID);
pfn_metadata* get_free_pfn(VOID);

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
VOID PTE_SetAccessedBit(PPTE pte);
VOID PTE_ClearAccessedBit(PPTE pte);
VOID AgeListTick(VOID);

// Initialization (init.c)
VOID init_lists(VOID);
VOID init_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);
VOID init_disc(VOID);
VOID init_global_locks(VOID);
VOID init_pte_regions(VOID);
VOID init_events(VOID);
VOID init(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);

// Diagnostics
VOID print_age_list_histogram(VOID);
VOID print_statistics(VOID);
static __forceinline void stat_add(MM_STAT* s, LONG64 t, LONG64 tsub, LONG64 pages);
static __forceinline LONG64 QPC(void);

// Privilege / setup (init.c)
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
DWORD WINAPI periodic_thread(PVOID parameter);

// Test driver
VOID full_virtual_memory_test(VOID);
