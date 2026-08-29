// pfn.h : physical frame metadata, the free-page magazines, frame acquisition + the trimmer
#pragma once
#include "vm.h"

// Physical frame metadata via sparse array indexed by frame number
extern pfn_metadata* physical_slots;
extern ULONG64       max_frame_number;

// Per-thread free-page caches (hot path)
extern FREE_PAGE_CACHE free_caches[NUM_THREADS];
extern volatile LONG64 cached_pages;

// Frame lookup
pfn_metadata* get_pfn_from_PListEntry(PLIST_ENTRY entry);
pfn_metadata* get_pfn_from_fn(ULONG64 fn);

// Frame acquisition
pfn_metadata* get_pfn_from_zero(VOID);
pfn_metadata* get_pfn_from_standby(VOID);
pfn_metadata* get_pfn_for_fault(BOOL from_disc);
ULONG         refill_free_cache(FREE_PAGE_CACHE* cache, int shard);
int           free_shard_for_thread(VOID);

// Trimmer: retrieve active frames off the region age lists onto modified list
VOID get_unmap_candidates_and_trim(int* batch_count, INT batch_size);

// Standby rescue helper
VOID claim_rescued_pfn(pfn_metadata* pfn);

// Frame / staging helpers
VOID       validate_page_contents(PVOID page_va, const char* site);
BOOL       pool_is_empty(VOID);
VOID       scrub_frame(pfn_metadata* pfn);
PULONG_PTR stage_ring_map(ULONG64 frame_number);
PULONG_PTR stage_ring_map_batch(ULONG_PTR* frames, ULONG d);
VOID       stage_ring_flush(VOID);
