// globals.c : Definitions for the shared global state of the ZoeVM memory manager.
//
// vm.h declares all of these `extern`; this is the single translation unit
// that actually defines them. Keeping them here instead of in a header avoids
// duplicate-symbol link errors, and keeps vm.c to code only.

#include "vm.h"

// Locks
ULONG64 NUM_PTE_LOCKS = 0;

// PTE list heads
LIST_HEAD freeList_head;
LIST_HEAD activeList_head;
LIST_HEAD modifiedList_head;
LIST_HEAD standbyList_head;
LIST_HEAD zeroList_head;

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

// Physical frames
pfn_metadata* physical_slots = NULL;
ULONG64 max_frame_number = 0;

// Disc
PDISC_METADATA disc_metadata;
ULONG64* disc_free_stack;
volatile LONG64 disc_stack_top;
CRITICAL_SECTION disc_stack_lock;
PVOID disc;
ULONG64 disc_page_count;

// Counters
volatile LONG64 va_access_count;
volatile LONG64 tick_call;
volatile LONG64 disk_debug[32];
volatile LONG64 hard_fault_count = 0;
volatile LONG64 soft_fault_count = 0;

// Other globals
PULONG_PTR VA_SPACE;
PVOID temp_va_base;
ULONG_PTR virtual_address_size_in_unsigned_chunks;
PULONG_PTR physical_page_numbers;

__declspec(thread) int thread_index = -1;

ULONG64 last_age_tick = 0;
ULONG64 age_cursor = 0;
