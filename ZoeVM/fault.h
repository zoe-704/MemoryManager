// fault.h : fault handlers, worker-thread coordination (events + control vars), thread entries.
#pragma once
#include "vm.h"

// Events coordinating the fault threads and the trim / disc / age / zero workers.
extern HANDLE startAge_event;
extern HANDLE startTrim_event;      // a starved fault asks the trimmer for pages
extern HANDLE startZero_event;      // ask the zero thread to refill the zero reserve
extern HANDLE diskReady_event;      // disk has free slots
extern HANDLE modifiedReady_event;  // trimmer produced pages onto the modified list
extern HANDLE redoFault_event;      // producers tell fault threads pages are available / retry
extern HANDLE shutdown_event;       // manual-reset: tells every worker to exit

// Trim / age control set by the periodic thread.
extern volatile LONG64 g_trim_target;
extern volatile LONG64 g_trim_full_throttle;
extern volatile LONG64 g_age_regions_per_tick;

// Per-thread RNG for the workload generator.
VOID    SeedRng(THREAD_RNG_STATE* rng);
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng);

// Fault handlers. The faulting VA fully determines pte / region / page-aligned VA, so the caller
// derives them once and threads them in rather than re-deriving in every handler.
BOOLEAN handle_soft_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
BOOL    handle_hard_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va);
// Run-aware hard fault: PREFETCHES the contiguous run [page_aligned_va, +run_len) that the workload
// is about to touch, resolving up to a whole run's worth of hard faults with ONE batch map (and one
// staged disc read) instead of one access-violation + map per page. Falls back to handle_hard_fault
// for the single page when the pool is dry. run_len is the pages remaining in the current run.
BOOL    handle_hard_fault_run(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va, ULONG64 run_len);
// Fault dispatcher used by both workload threads: snapshots the PTE and routes to the soft or hard
// handler. run_len is the pages remaining in the current run (drives hard-fault prefetch); pass 0 to
// disable prefetch (the random workload has no runs). The caller always retries the same VA after.
VOID    handle_page_fault(PVOID arbitrary_va, PPTE pte, ULONG64 run_len);
ULONG   write_modified_list(VOID);   // returns # frames drained this pass (0 == nothing eligible)

// Thread entry points.
DWORD WINAPI page_fault_thread(PVOID parameter);
DWORD WINAPI trim_thread(PVOID parameter);
DWORD WINAPI disc_thread(PVOID parameter);
DWORD WINAPI age_thread(PVOID parameter);
DWORD WINAPI periodic_thread(PVOID parameter);

// Test driver.
VOID full_virtual_memory_test(VOID);
