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
// mappings to a single physical page.
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
#define AGE_SLICE_DIVISOR 8

#define MIN_AGE_INTERVAL_MS 10

#define WRITE_BATCH 16

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
    };
} PTE, * PPTE;

typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    ULONG64 active_page_count;
    ULONG64 age_counts[8];      // track number of valid PTEs in region with a certain age
} PTE_REGION, * PPTE_REGION;

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

// Struct for disc metadata
typedef struct _DISC_METADATA {
    LIST_ENTRY list;
    ULONG64 index;
    BOOL isOccupied;
} DISC_METADATA, * PDISC_METADATA;

typedef struct _THREAD_RNG_STATE {
    ULONG64 s0;
    ULONG64 s1;
    unsigned int counter;
} THREAD_RNG_STATE;

// ---- Global state (defined in vm.c) ----

extern ULONG64 NUM_PTE_LOCKS;

extern LIST_HEAD freeList_head;
extern LIST_HEAD activeList_head;
extern LIST_HEAD modifiedList_head;
extern LIST_HEAD standbyList_head;

extern BOOL trim_running;
extern HANDLE startAge_event;
extern HANDLE startTrim_event;
extern HANDLE diskReady_event;
extern HANDLE modifiedReady_event;
extern HANDLE redoFault_event;
extern HANDLE shutdown_event;


extern PPTE page_table;
extern PPTE_REGION pte_regions;
extern ULONG64 num_ptes;

extern PULONG_PTR VA_SPACE;
extern PVOID temp_va_base;
extern __declspec(thread) int thread_index;
extern ULONG_PTR virtual_address_size_in_unsigned_chunks;
extern PULONG_PTR physical_page_numbers;

extern pfn_metadata* physical_slots;
extern ULONG64 max_frame_number;

extern PDISC_METADATA disc_metadata;
extern ULONG64* disc_free_stack;          // array of free slot indices
extern volatile LONG64 disc_stack_top;    // index of next push slot == current count of free slots
extern CRITICAL_SECTION disc_stack_lock;  // keep the lock for now (bitmap/lock-free is a later step)
extern PVOID disc;

extern ULONG64 disc_page_count;

extern volatile LONG64 va_access_count;
extern volatile LONG64 tick_call;
extern volatile LONG64 disk_debug[32];

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
