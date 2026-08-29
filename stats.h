// stats.h : counters, per-thread statistics, and diagnostic dumps
#pragma once
#include "vm.h"

// Thread idex
extern __declspec(thread) int thread_index;

// General counters
extern volatile LONG64 tick_call;
extern volatile LONG64 disk_debug[32];

// Per-thread lock-free statistics
extern THREAD_STATS thread_stats[NUM_THREADS];
extern __declspec(thread) THREAD_STATS* g_my_stats;
LONG64 total_hard_faults(VOID);
LONG64 total_soft_faults(VOID);

// Timed-region accumulators
extern MM_STAT       g_trim_stat;
extern MM_STAT       g_write_stat;
extern TRIM_PROFILE  g_trim_prof;
extern LARGE_INTEGER g_qpc_freq;

// Diagnostic dumps
VOID print_statistics(VOID);
VOID print_age_list_histogram(VOID);
