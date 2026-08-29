// globals.c : Definitions for the shared global state of the ZoeVM memory manager.
// vm.h declares all of these `extern` and define them here

#include "vm.h"
#include "globals.h"

// Locks
ULONG64 NUM_PTE_LOCKS = 0;

// PTE list heads
LIST_HEAD freeList_shards[NUM_FAULT_THREADS];
CONCURRENT_LIST_HEAD modifiedList_head;
CONCURRENT_LIST_HEAD standbyList_head;
LIST_HEAD zeroList_head;

// Per-thread free-page caches
FREE_PAGE_CACHE free_caches[NUM_THREADS];
volatile LONG64 cached_pages = 0;

// Events
BOOL trim_running = TRUE;
HANDLE startAge_event;
HANDLE startTrim_event;
HANDLE startZero_event;
HANDLE diskReady_event;
HANDLE modifiedReady_event;
HANDLE redoFault_event;
HANDLE shutdown_event;

// Page table
PPTE page_table;
PPTE_REGION pte_regions;
ULONG64 num_ptes;

// Region-age index
REGION_AGE_LIST region_age_lists[AGES];
CRITICAL_SECTION region_age_lock;

// Physical frames
pfn_metadata* physical_slots = NULL;
ULONG64 max_frame_number = 0;

// Disc
PVOID disc;
ULONG64 disc_page_count;
volatile LONG64* disc_bitmap;
ULONG64          disc_bitmap_rows;
volatile LONG64  g_disc_free_count;

// Counters
volatile LONG64 tick_call;
volatile LONG64 disk_debug[32];

// Per-thread stats: one slot per thread
// g_my_stats points each thread at its own slot
THREAD_STATS thread_stats[NUM_THREADS] = { 0 };
__declspec(thread) THREAD_STATS* g_my_stats = NULL;

// Other globals
PULONG_PTR VA_SPACE;
PVOID temp_va_base;
ULONG_PTR virtual_address_size_in_unsigned_chunks;
PULONG_PTR physical_page_numbers;
SIZE_T scratch_bytes = (SIZE_T)NUM_THREADS * THREAD_SCRATCH_PAGES * PAGE_SIZE;
volatile LONG64 g_trim_target = 0;
volatile LONG64 g_trim_full_throttle = 0;   // 1 when consume_rate > trim_rate (trimmer max speed)
__declspec(thread) int thread_index = -1;

// Aging
ULONG64 last_age_tick = 0;
ULONG64 age_cursor = 0;
volatile LONG64 g_age_regions_per_tick = 1;   // clock-hand advances tuned by periodic thread

