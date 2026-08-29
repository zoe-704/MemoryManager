// pte.h : the page table, PTE regions, region-age index, and PTE/region operations
#pragma once
#include "vm.h"

// Page table + per-region metadata
extern ULONG64      NUM_PTE_LOCKS;
extern PPTE         page_table;
extern PPTE_REGION  pte_regions;
extern ULONG64      num_ptes;

// Region-age index: regions bucketed by oldest active page
// One lock guards all region's lists
extern REGION_AGE_LIST region_age_lists[AGES];
extern CRITICAL_SECTION region_age_lock;

// Aging cursor
extern ULONG64 last_age_tick;
extern ULONG64 age_cursor;

// PTE -- VA -- region getters
PPTE_REGION       get_pte_region(PPTE pte);
CRITICAL_SECTION* get_pte_lock(PPTE pte);
PPTE              get_pte_from_va(PULONG_PTR va);
PULONG_PTR        get_va_from_pte(PPTE pte);

// PTE setters
VOID set_pte_valid(PPTE pte, ULONG64 frame_number, ULONG64 age);
VOID set_pte_invalid(PPTE pte);
VOID set_pte_transition(PPTE pte, ULONG64 frame_number);
VOID set_pte_disc(PPTE pte, ULONG64 disc_index);

// Access-bit / aging helpers
BOOLEAN PTE_WasAccessed(PPTE pte);
VOID    PTE_SetAccessedBit(PPTE pte);
VOID    PTE_ClearAccessedBit(PPTE pte);
VOID    pte_set_accessed(PPTE pte);

// Region active-list and region-age index helpers
// Place frame onto age list, return highest age with non-empty bucket, rebucket regions
VOID    region_add_active(PPTE_REGION region, pfn_metadata* pfn, ULONG64 age);
ULONG64 region_oldest_age(PPTE_REGION region);
VOID    region_rebucket(PPTE_REGION region);

// Repurpose a standby frame by repointing its owner PTE to disc
BOOLEAN rescue_standby_to_disc(pfn_metadata* pfn);
