// init.h : one-time setup for virtual address space, page-file, privilege, and the init routines
#pragma once
#include "vm.h"

// Virtual address space + scratch/staging globals set up at startup
extern PULONG_PTR VA_SPACE;
extern PVOID      temp_va_base;
extern ULONG_PTR  virtual_address_size_in_unsigned_chunks;
extern PULONG_PTR physical_page_numbers;
extern SIZE_T     scratch_bytes;

// Allocation helper
PVOID zero_malloc(size_t num_bytes);

// Initialization 
VOID init_lists(VOID);
VOID init_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);
VOID init_disc(VOID);
VOID init_pte_regions(VOID);
VOID init_events(VOID);
VOID init_free_caches(VOID);
VOID init(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers);

// Privilege + program setup
BOOL GetPrivilege(VOID);
#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
HANDLE CreateSharedMemorySection(VOID);
#endif
BOOL setup_program(VOID);
