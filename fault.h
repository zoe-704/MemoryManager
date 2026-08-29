// fault.h : fault handlers, worker-thread coordination (events + control vars), thread entries
#pragma once
#include "vm.h"

// Events coordinating fault threads and workers
extern HANDLE startAge_event;
extern HANDLE startTrim_event;      // a starved fault asks the trimmer for pages
extern HANDLE startZero_event;      // ask the zero thread to refill the zero reserve
extern HANDLE diskReady_event;      // disk has free slots
extern HANDLE modifiedReady_event;  // trimmer produced pages onto the modified list
extern HANDLE redoFault_event;      // manual-reset: producers tell fault threads pages are available / retry
extern HANDLE shutdown_event;       // manual-reset: tells every worker to exit

// Trim / age control set by the periodic thread
extern volatile LONG64 g_trim_target;
extern volatile LONG64 g_trim_full_throttle;
extern volatile LONG64 g_age_regions_per_tick;

// Per-thread RNG for the workload generator
VOID SeedRng(THREAD_RNG_STATE* rng);
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng);

// Fault handlers
BOOLEAN handle_soft_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
BOOL    handle_hard_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
// Prefetches contiguous run
BOOL    handle_hard_fault_run(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va, ULONG64 run_len);

VOID    handle_page_fault(PVOID arbitrary_va, PPTE pte, ULONG64 run_len);
ULONG   write_modified_list(VOID);   // returns # frames drained this pass 

// Thread entry points
DWORD WINAPI page_fault_thread(PVOID parameter);
DWORD WINAPI trim_thread(PVOID parameter);
DWORD WINAPI disc_thread(PVOID parameter);
DWORD WINAPI age_thread(PVOID parameter);
DWORD WINAPI periodic_thread(PVOID parameter);

// Test driver
VOID full_virtual_memory_test(VOID);
