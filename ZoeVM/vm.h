// vm.h : Shared declarations

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 1

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

#define PAGE_SIZE                   4096

#define KB(x)                       ((x) * 1024)
#define MB(x)                       ((x) * 1024 * 1024)
#define GB(x)                       ((x) * 1024 * 1024 * 1024)

// Use physical page pool ~1% of VA space
#define NUMBER_OF_PHYSICAL_PAGES    (1 * GB(1) / PAGE_SIZE)
#define NUM_DISC_PAGES              (3 * NUMBER_OF_PHYSICAL_PAGES)
#define MAX_DISC_PTE_BITS           40
#define MAX_DISC_SIZE               ((ULONG64) 1 << MAX_DISC_PTE_BITS)
#define INVALID_DISC_SLOT           ((1ULL << MAX_DISC_PTE_BITS) - 1)
#define MB_MUL                      100
#define MB_DIV                      1

// Number of threads
#define NUM_FAULT_THREADS   7
#define NUM_WORKER_THREADS  5
#define NUM_THREADS         (NUM_FAULT_THREADS + NUM_WORKER_THREADS)

// Page thresholds
#define MAX_TRIM_PAGES              512
#define MIN_TRIM_BATCH              32
#define LOW_ZERO_PAGE_THRESHOLD     (NUMBER_OF_PHYSICAL_PAGES / 256)   // wake the zero thread below this
#define HIGH_ZERO_PAGE_THRESHOLD    (LOW_ZERO_PAGE_THRESHOLD * 4)      // stop zeroing at this reserve
#define LOW_FREE_PAGE_THRESHOLD     (NUMBER_OF_PHYSICAL_PAGES / 64)
#define POOL_HIGH_WATER             (NUMBER_OF_PHYSICAL_PAGES / 16)    // stop trimming once ready pool is this level (lower = more active pages)
#define PTES_PER_LOCK               512

// Disc thresholds
#define DISC_WRITE_BATCH        64
#define DISC_REGIONS            64
#define DISC_SLOTS_PER_REGION   (disc_page_count / DISC_REGIONS)

// List types
#define LIST_NONE      0   // in-transit or free/zero
#define LIST_ACTIVE    1
#define LIST_MODIFIED  2
#define LIST_STANDBY   3

// Free cache and shards
#define NUM_FREE_SHARDS     NUM_FAULT_THREADS   // one free-list shard per fault threa
#define FREE_CACHE_MAX      64                  // per-thread free-page cache
#define CACHE_REFILL_BATCH  32                  // pages pulled from a shard into the cache per refill

// Enable non-random VA access
#define HOT_ZONE_BIAS         10    // 1-in-N cold jumps reach the wide slice (rest stay in the hot zone)
#define HOT_ZONE_DIVISOR      16    // hot zone = slice/this
#define WORKING_SET_DIVISOR   2     // cold jumps confined to total_pages/this 
#define HOT_SPOTS             16    // remembered spots per thread
#define REVISIT_CHANCE        3     // 1-in-N chance of revisiting a hot spot
#define RECENT_BIAS           2     // 1-in-N of revisits pick from newest 4
#define MIN_RUN_PAGES         4
#define MAX_RUN_PAGES         32

// Ages and consumption rate thresholds
#define AGES                    8
#define REGION_AGE_NONE         AGES   // region has no active pages so not on region-age list
#define AGE_TICK_MS             25   // fixed cadence of the age thread
#define AGE_ALPHA               1.0  // >1 ages faster
#define AGE_TSWEEP_MIN_MS       25   // cap under heavy pressure
#define AGE_TSWEEP_BASELINE_MS  50    
#define TARGET_REMAINING_S      0.5
#define MODIFIED_HIGH_WATER     (NUMBER_OF_PHYSICAL_PAGES / 8)

// Allocate and access each thread's chunk of temp_va_base
#define STAGE_RING_PAGES     512
#define SLOT_SCRUB           0
#define OFF_STAGE            1
#define OFF_WRITE            (OFF_STAGE + STAGE_RING_PAGES)
#define THREAD_SCRATCH_PAGES (OFF_WRITE + DISC_WRITE_BATCH)

// For debugging and performance
#define DEBUG 0
#define STATISTICS 0

#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

// Struct for coarse-locked lists
typedef struct _LIST_HEAD_ENTRY {
    LIST_ENTRY entry;
    CRITICAL_SECTION list_lock;
    ULONG64 list_count;
} LIST_HEAD, * PLIST_HEAD;

// Struct for fine-grained concurrent lists
typedef struct _CONCURRENT_LIST_HEAD {
    LIST_ENTRY entry;
    SRWLOCK srw;                  // shared = parallel removers, exclusive = inserter
    CRITICAL_SECTION head_lock;  
    ULONG64 list_count;      
} CONCURRENT_LIST_HEAD, * PCONCURRENT_LIST_HEAD;

// Walk cursor for scanning a CONCURRENT_LIST_HEAD under shared acces and skip contended nodes
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

typedef struct _HOT_SPOT {
    ULONG64 base;
    ULONG64 len;
} HOT_SPOT;

typedef struct _FREE_PAGE_CACHE {
    ULONG64 count;
    pfn_metadata* pages[FREE_CACHE_MAX];
} FREE_PAGE_CACHE;

extern FREE_PAGE_CACHE free_caches[NUM_THREADS];     // per thread reachable by trimmer
extern volatile LONG64 cached_pages;                 // pages currently parked in caches

// Per-category counter block
typedef struct _MM_STAT {
    volatile LONG64 calls;
    volatile LONG64 ticks;      // raw QPC ticks
    volatile LONG64 ticks_sub;  // secondary timed region
    volatile LONG64 pages;
} MM_STAT;

// Trimmer profile
typedef struct _TRIM_PROFILE {
    volatile LONG64 claim_ticks;     // Phase 1: claim victims off active buckets
    volatile LONG64 unmap_ticks;     // Phase 2: MapUserPhysicalPagesScatter syscall
    volatile LONG64 publish_ticks;   // Phase 3: pfn lock + RWListInsertTail (modified SRW, exclusive)
    volatile LONG64 region_tryfail;  // region->lock TryEnter failures
    volatile LONG64 region_enter;    // regions successfully locked and trimmed
    volatile LONG64 batch_calls;     // trim_region calls
} TRIM_PROFILE;

// Per-thread statisticsts
typedef __declspec(align(64)) struct _THREAD_STATS {
    LONG64 hard_faults;   
    LONG64 soft_faults;   // soft faults resolved
    LONG64 hard_disc;     // hard faults: disc-backed
    LONG64 hard_zero;     // hard faults: demand-zero
    LONG64 fault_waits;   // times fault stalled waiting for page pool
    LONG64 trims;         // pages trimmed active->modified
    LONG64 writes;        // pages written modified->standby
    LONG64 zeroes;        // pages zeroed onto zero list
} THREAD_STATS;        

typedef struct _THREAD_RNG_STATE {
    ULONG64 s0;
    ULONG64 s1;
    unsigned int counter;
} THREAD_RNG_STATE;


// Small shared inline helpers for PFNs
static __forceinline VOID lock_pfn(pfn_metadata* p)   { EnterCriticalSection(&p->lock); }
static __forceinline VOID unlock_pfn(pfn_metadata* p) { LeaveCriticalSection(&p->lock); }

// File-specific declarations
#include "list.h"
#include "pte.h"
#include "pfn.h"
#include "disc.h"
#include "stats.h"
#include "fault.h"
#include "init.h"
