// disc.h : page-file (disc) state + the lock-free slot bitmap allocator
#pragma once
#include "vm.h"

// The fake page file and its size
extern PVOID          disc;
extern ULONG64        disc_page_count;

// Lock-free disc-slot allocator: one bit per slot (1 = used) with 64 in a row
// Frees are atomic bit-clears from any thread 
// Allocation (single disc thread) grabs whole free rows with one CAS into a thread-local stash 
extern volatile LONG64* disc_bitmap;
extern ULONG64          disc_bitmap_rows;
extern volatile LONG64  g_disc_free_count;

PVOID   create_page_file(PULONG64 number_of_pages);
ULONG64 get_disk_free_slots(VOID);
VOID    return_disk_free_slots(ULONG64 slot);