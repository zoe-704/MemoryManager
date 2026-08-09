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

#define NUMBER_OF_PHYSICAL_PAGES    (1 * GB(1) / PAGE_SIZE)
#define NUM_DISC_PAGES              (3 * NUMBER_OF_PHYSICAL_PAGES)
#define MAX_DISC_PTE_BITS           40
#define MAX_DISC_SIZE               ((ULONG64) 1 << MAX_DISC_PTE_BITS)
#define INVALID_DISC_SLOT           ((1ULL << MAX_DISC_PTE_BITS) - 1)

#define NUM_FAULT_THREADS   7
#define NUM_THREADS         (NUM_FAULT_THREADS + 5)   // 5 workers: trim, disc, age, periodic, zero

#define MAX_TRIM_PAGES  512
#define MIN_TRIM_BATCH  32
#define LOW_ZERO_PAGE_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES / 256)   // wake the zero thread below this
#define ZEROED_LIST_HIGH        (LOW_ZERO_PAGE_THRESHOLD * 4)      // stop zeroing at this reserve
#define LOW_FREE_PAGE_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES / 64)

#define WRITE_BATCH             64
#define PTES_PER_LOCK           512
#define DISC_REGIONS            64
#define DISC_SLOTS_PER_REGION   (disc_page_count / DISC_REGIONS)

#define LIST_NONE      0   // in transit, or on free/zero
#define LIST_ACTIVE    1
#define LIST_MODIFIED  2
#define LIST_STANDBY   3

#define MB_MUL                  100
#define MB_DIV                  1
#define AGES                    8
#define AGE_SLICE_DIVISOR       4
#define HOT_ZONE_BIAS           10   // 1-in-N cold jumps reach the wide slice; the rest stay in the hot zone
#define HOT_ZONE_DIVISOR        16   // hot zone = slice / this
#define AGE_TICK_MS             25   // fixed fast cadence of the age thread
#define AGE_ALPHA               1.0  // T_sweep = V/(ALPHA*C); >1 ages faster
#define AGE_TSWEEP_MIN_MS       25   // cap under heavy pressure
#define AGE_TSWEEP_BASELINE_MS  50      

#define WORKING_SET_DIVISOR   2     // cold jumps confined to total_pages/this 
#define HOT_SPOTS             16    // remembered spots per thread
#define REVISIT_CHANCE        3     // 1-in-N chance of revisiting a hot spot
#define RECENT_BIAS           2     // 1-in-N of revisits pick from newest 4 
#define MIN_RUN_PAGES         4
#define MAX_RUN_PAGES         32
#define TARGET_REMAINING_S    0.5
#define MODIFIED_HIGH_WATER   (NUMBER_OF_PHYSICAL_PAGES / 8)

#define STAGE_RING_PAGES     512
#define SLOT_SCRUB           0
#define OFF_STAGE            1
#define OFF_WRITE            (OFF_STAGE + STAGE_RING_PAGES)
#define THREAD_SCRATCH_PAGES (OFF_WRITE + WRITE_BATCH)

#define FREE_CACHE_MAX      64                  // per-thread free-page magazine capacity
#define NUM_FREE_SHARDS     NUM_FAULT_THREADS   // one free-list shard per fault thread (8)
#define FREE_REFILL_BATCH   32                  // pages pulled from a shard into the cache per refill

#define LIST_COUPLE_MAX_FAILS 4   // consecutive neighbor-lock fails before a lock-coupled RW op escalates to exclusive

#define DEBUG 0
#define STATISTICS 1   // 0 = compile out QPC timing in the trim/write hot paths for max speed

#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

// ---- Types ----

// Struct for coarse-locked lists (free shards + zero)
typedef struct _LIST_HEAD_ENTRY {
    LIST_ENTRY entry;
    CRITICAL_SECTION list_lock;
    ULONG64 list_count;
} LIST_HEAD, * PLIST_HEAD;

// Struct for fine-grained concurrent lists (standby + modified)
typedef struct _CONCURRENT_LIST_HEAD {
    LIST_ENTRY entry;
    SRWLOCK srw;                  // shared = parallel removers, exclusive = inserter
    CRITICAL_SECTION head_lock;   // stands in as the anchor entry's per-node lock
    ULONG64 list_count;           // updated with Interlocked under shared srw
} CONCURRENT_LIST_HEAD, * PCONCURRENT_LIST_HEAD;

// Hand-over-hand walk cursor for scanning a CONCURRENT_LIST_HEAD under shared access,
// skipping contended nodes instead of escalating (Policy B)
typedef struct _RW_LIST_CURSOR {
    PCONCURRENT_LIST_HEAD list;
    PLIST_ENTRY       prev;
    CRITICAL_SECTION* prev_lock;
    PLIST_ENTRY       cur;
    CRITICAL_SECTION* cur_lock;
    BOOLEAN           forward;
} RW_LIST_CURSOR;

// Struct for our PTEs
typedef struct _VALID_PTE {
    ULONG64 valid : 1;
    ULONG64 accessed : 1;
    ULONG64 age : 3;
    ULONG64 frame_number : 40;
    ULONG64 reserved : 19;
} VALID_PTE, * PVALID_PTE;

typedef struct _TRANSITION_PTE {
    ULONG64 valid : 1;          // = 0
    ULONG64 transition : 1;     // = 1
    ULONG64 frame_number : 40;
    ULONG64 reserved : 22;
} TRANSITION_PTE, * PTRANSITION_PTE;

typedef struct _DISC_PTE {
    ULONG64 valid : 1;          // = 0
    ULONG64 transition : 1;     // = 0
    ULONG64 disc : 1;           // = 1
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

// Region-granular aging + per-region active lists.
//
// Coherence invariant: an ACTIVE frame is always on exactly one list -- its region's
// active_age_lists[age]. The trimmer CLAIMS a frame by RemoveEntryList off that list under
// region->lock (an ownership handoff), never by dereferencing a PTE's frame number. That is
// what keeps the modified/standby RWLists fed only coherent frames (VMM's invariant).
//
// Two independent indices:
//   * active_age_lists[age] (+ age_counts[age] = its length) -- the region's ACTIVE frames,
//     bucketed by age. Guarded by region->lock.
//   * region_age_lists[age]  -- whole regions bucketed by their OLDEST active page, for fast
//     trimmer region-selection. Guarded by region_age_lock. A region's age_list_number names
//     which one it is on, or REGION_AGE_NONE when it has no active pages.
#define REGION_AGE_NONE   AGES   // region has no active pages -> on no region-age list

// Struct for PTE regions
typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    ULONG64 active_page_count;
    LIST_ENTRY active_age_lists[AGES];  // this region's ACTIVE frames, bucketed by age
    ULONG64 age_counts[AGES];           // length of each bucket (O(1) region age)
    // --- region-age-list membership; guarded by region_age_lock, NOT region->lock ---
    LIST_ENTRY age_link;                // this region's node on region_age_lists[age_list_number]
    ULONG64    age_list_number;         // which region-age list, or REGION_AGE_NONE
} PTE_REGION, * PPTE_REGION;

// One list of REGIONS per age level (regions here all share the same oldest-page age).
typedef struct _REGION_AGE_LIST {
    LIST_ENTRY head;
    ULONG64    count;
} REGION_AGE_LIST;

// Struct for our PFNs
typedef struct _pfn_metadata {
    LIST_ENTRY links;           // free / active / modified / standby list
    CRITICAL_SECTION lock;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index : MAX_DISC_PTE_BITS;
    // State bits. They pack into the same 8-byte word as disc_index above.
    // INVARIANT: every field in this word (disc_index, list_type, being_written,
    // accessed) is only read-decided-written while holding THIS frame's `lock` -- or
    // while the frame is exclusively owned and off every list (freshly allocated from
    // free/zero, or just rescued off standby, before it is republished). There is no
    // lock-free path: the frame lock serializes the whole word, so plain bitfield
    // writes from different paths cannot tear it. Unlocked reads are peeks only and
    // are always re-validated under the lock before any decision is committed.
    ULONG64 list_type : 2;
    ULONG64 being_written : 1;
    ULONG64 accessed : 1;
    ULONG64 owner_thread_id;
    BOOLEAN is_zero;
} pfn_metadata;

// Struct for disc metadata
typedef struct _DISC_METADATA {
    LIST_ENTRY list;
    ULONG64 index;
    BOOL isOccupied;
} DISC_METADATA, * PDISC_METADATA;

typedef struct _DISC_REGION {
    CRITICAL_SECTION lock;      // ZS 1 bit?
    ULONG64 base_slot;
    ULONG64 slot_count;
    ULONG64 free_count;
    ULONG64 cursor;
    BYTE* bytemap;
} DISC_REGION, *PDISC_REGION;

typedef struct _HOT_SPOT {
    ULONG64 base;
    ULONG64 len;
} HOT_SPOT;

typedef struct _FREE_PAGE_CACHE {
    ULONG64 count;
    pfn_metadata* pages[FREE_CACHE_MAX];
} FREE_PAGE_CACHE;

extern FREE_PAGE_CACHE free_caches[NUM_THREADS];     // one per thread, reachable by the trimmer
extern volatile LONG64 cached_pages;                 // pages currently parked in caches

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
extern LIST_HEAD freeList_shards[NUM_FAULT_THREADS];
extern CONCURRENT_LIST_HEAD modifiedList_head;
extern CONCURRENT_LIST_HEAD standbyList_head;
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

// Region-age index: regions bucketed by their oldest active page. One global lock guards all
// AGES lists AND every region's age_link / age_list_number (region moves are amortized/rare,
// so a single lock is right and deadlock-free).
extern REGION_AGE_LIST region_age_lists[AGES];
extern CRITICAL_SECTION region_age_lock;

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
extern PDISC_REGION disc_regions;         // array of DISC_REGIONS

// Counters
extern volatile LONG64 va_access_count;
extern volatile LONG64 loop_iterations;
extern volatile LONG64 tick_call;
extern volatile LONG64 disk_debug[32];
// ZS decelspec global struct
extern volatile LONG64 hard_fault_count;
extern volatile LONG64 soft_fault_count;

// Per-category counter block
extern MM_STAT g_trim_stat;
extern MM_STAT g_write_stat;
extern LARGE_INTEGER g_qpc_freq;   // set once via QueryPerformanceFrequency

// Other globals
extern PULONG_PTR VA_SPACE;
extern PVOID temp_va_base;
extern ULONG_PTR virtual_address_size_in_unsigned_chunks;
extern PULONG_PTR physical_page_numbers;
extern SIZE_T scratch_bytes;
extern __declspec(thread) int thread_index;

extern ULONG64 last_age_tick;
extern ULONG64 age_cursor;      // only age_thread touches it for now
extern volatile LONG64 g_trim_target;
extern volatile LONG64 g_trim_full_throttle;
extern volatile LONG64 g_age_regions_per_tick;

// ---- Function prototypes ----

// RNG
VOID SeedRng(THREAD_RNG_STATE* rng);
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng);

// Linked list primitives
VOID InitializeListHead(PLIST_ENTRY ListHead);
VOID InitializeList(PLIST_HEAD Head);
VOID InitializeConcurrentList(PCONCURRENT_LIST_HEAD Head);
BOOLEAN IsListEmpty(PLIST_ENTRY ListHead);
VOID InsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
VOID InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN RemoveEntryList(PLIST_ENTRY Entry);

// Concurrent-list access: pfn-lock + lock-coupled neighbors under a shared SRW,
// escalating to exclusive when coupling keeps failing (see vm.c for the protocol).
VOID RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node);
VOID RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node);
pfn_metadata* RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward);
pfn_metadata* RWListScanNext(RW_LIST_CURSOR* c);
pfn_metadata* RWListScanRemoveCurrent(RW_LIST_CURSOR* c);
VOID RWListScanEnd(RW_LIST_CURSOR* c);
VOID check_concurrent_list(PCONCURRENT_LIST_HEAD L, ULONG64 expected_type, const char* name);

// Allocation helpers
PVOID zero_malloc(size_t num_bytes);

static BOOL try_lock_window(PLIST_HEAD L, PLIST_ENTRY a, PLIST_ENTRY b, PLIST_ENTRY c);
static BOOL list_remove_locked(PLIST_HEAD L, pfn_metadata* pfn);
static BOOL list_insert_tail_locked(PLIST_HEAD L, pfn_metadata* pfn);

// Getters
PPTE_REGION get_pte_region(PPTE pte);
CRITICAL_SECTION* get_pte_lock(PPTE pte);
PPTE get_pte_from_va(PULONG_PTR va);
PULONG_PTR get_va_from_pte(PPTE pte);
pfn_metadata* get_pfn_from_PListEntry(PLIST_ENTRY entry);
pfn_metadata* get_pfn_from_fn(ULONG64 fn);

// Region active-list + region-age index helpers.
// region_add_active: put a just-activated frame onto region->active_age_lists[age].
//   Caller holds region->lock AND pfn->lock. Re-buckets the region if its oldest age changed.
// region_oldest_age: highest age with a non-empty bucket, or REGION_AGE_NONE. Caller holds region->lock.
// region_rebucket: move the region to the region-age list matching its current oldest age.
//   Caller holds region->lock (reads age_counts); takes region_age_lock internally.
VOID region_add_active(PPTE_REGION region, pfn_metadata* pfn, ULONG64 age);
ULONG64 region_oldest_age(PPTE_REGION region);
VOID region_rebucket(PPTE_REGION region);

// Trim / free-page acquisition
VOID get_unmap_candidates_and_trim(int* batch_count, INT batch_size);
pfn_metadata* get_pfn_from_zero(VOID);
pfn_metadata* get_pfn_from_standby(VOID);
// Hot/cold acquisition: hot path is a lock-free thread-local magazine pop; the cold path refills
// from the shards / steals from standby. from_disc == the fault overwrites the frame from disc,
// so it needs no zeros and never touches the zero list.
pfn_metadata* get_pfn_for_fault(BOOL from_disc);

ULONG refill_free_cache(FREE_PAGE_CACHE* cache, int shard);

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

// Initialization (init.c)
VOID init_lists(VOID);
VOID init_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);
VOID init_disc(VOID);
VOID init_global_locks(VOID);
VOID init_pte_regions(VOID);
VOID init_events(VOID);
VOID init_free_caches(VOID);
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
// The faulting VA fully determines pte / region / page-aligned VA, so the caller derives them
// once and threads them in rather than every handler (and the hard->soft hand-off) recomputing.
BOOLEAN handle_soft_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
BOOL handle_hard_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
ULONG write_modified_list(VOID);   // returns # frames drained this pass (0 == nothing eligible)

// Thread entry points
DWORD WINAPI page_fault_thread(PVOID parameter);
DWORD WINAPI trim_thread(PVOID parameter);
DWORD WINAPI disc_thread(PVOID parameter);
DWORD WINAPI age_thread(PVOID parameter);
DWORD WINAPI periodic_thread(PVOID parameter);

// Test driver
VOID full_virtual_memory_test(VOID);

static __forceinline VOID lock_pfn(pfn_metadata* p)   { EnterCriticalSection(&p->lock); }
static __forceinline VOID unlock_pfn(pfn_metadata* p) { LeaveCriticalSection(&p->lock); }

// Cross-file prototypes (split moved these out of vm.c; they are called from other
// translation units so they need external linkage + a visible prototype here).
int free_shard_for_thread(VOID);
PULONG_PTR stage_ring_map(ULONG64 frame_number);
VOID stage_ring_flush(VOID);
BOOLEAN rescue_standby_to_disc(pfn_metadata* pfn);
VOID claim_rescued_pfn(pfn_metadata* pfn);
pfn_metadata* rwlist_scan_rescue_remove(RW_LIST_CURSOR* c);
VOID pte_set_accessed(PPTE pte);
