#include "vm.h"
#include "pte.h"

// Getters
// Getting pte regions
PPTE_REGION
get_pte_region(PPTE pte)
{
    ULONG64 pte_index = pte - page_table;
    ASSERT(pte >= page_table);
    ASSERT(pte_index / PTES_PER_LOCK < NUM_PTE_LOCKS);
    return &pte_regions[pte_index / PTES_PER_LOCK];
}

// Getting pte region locks
CRITICAL_SECTION*
get_pte_lock(PPTE pte)
{
    return &get_pte_region(pte)->lock;
}

// Gettinng PTE from VA
PPTE
get_pte_from_va(PULONG_PTR va)
{
    ULONG64 index = ((ULONG64)va - (ULONG64)VA_SPACE) / PAGE_SIZE;
    return page_table + index;
}

// Getting VA from PTE
PULONG_PTR
get_va_from_pte(PPTE pte)
{
    ULONG64 pte_index = pte - page_table;
    PULONG_PTR page_va = VA_SPACE + (pte_index * (PAGE_SIZE / sizeof(ULONG_PTR)));
    return (PULONG_PTR)((ULONG_PTR)page_va & ~(PAGE_SIZE - 1));
}


// Setters
// Set PTE to its valid state
VOID
set_pte_valid(PPTE pte, ULONG64 frame_number, ULONG64 age)
{
    PTE snapshot;
    // ZS use in other places read not necessary here
    // snapshot.entire_contents = ReadULong64NoFence(&pte->entire_contents);
    snapshot.entire_contents = 0;

    snapshot.hardware.valid = 1;
    snapshot.hardware.accessed = 1; // freshly faulted-in page counts as accessed but set 0 to start as cold
    snapshot.hardware.age = 0; // ZS only age?
    snapshot.hardware.frame_number = frame_number;
    snapshot.hardware.reserved = 0;

    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
}

// Zero out the PTE
VOID
set_pte_invalid(PPTE pte)
{
    WriteULong64NoFence((PLONG64)&pte->entire_contents, 0);
}

// Set PTE to its transistion state
VOID
set_pte_transition(PPTE pte, ULONG64 frame_number)
{
    PTE snapshot;
    snapshot.entire_contents = 0;

    snapshot.transition.valid = 0;
    snapshot.transition.transition = 1;
    snapshot.transition.frame_number = frame_number;
    snapshot.transition.reserved = 0;
    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
}

// Set PTE to its disc state
VOID
set_pte_disc(PPTE pte, ULONG64 disc_index)
{
    PTE snapshot;
    snapshot.entire_contents = 0;

    snapshot.disc.valid = 0;
    snapshot.disc.transition = 0;
    snapshot.disc.disc = 1;
    snapshot.disc.disc_index = disc_index;
    snapshot.disc.reserved = 0;
    WriteULong64NoFence(&pte->entire_contents, snapshot.entire_contents);
}

// Repurpose a standby frame by pointing its PTE at the disc copy and clearing the frame's pte/disc
// Caller holds the frame's lock
BOOLEAN
rescue_standby_to_disc(pfn_metadata* pfn)
{
    PPTE old_pte = pfn->pte;
    if (old_pte == NULL) {  // standby frame must have a pte
        DebugBreak();
        return FALSE;
    }
    CRITICAL_SECTION* region = get_pte_lock(old_pte);
    if (!TryEnterCriticalSection(region)) {
        return FALSE;
    }
    set_pte_disc(old_pte, pfn->disc_index);
    pfn->pte = NULL;
    pfn->disc_index = INVALID_DISC_SLOT;
    LeaveCriticalSection(region);
    return TRUE;
}

// Age stuff
VOID
pte_set_accessed(PPTE pte)
{
    for (;;) {
        ULONG64 old = *(volatile ULONG64*)pte;
        PTE s; *(ULONG64*)&s = old;
        if (s.hardware.valid != 1 || s.hardware.accessed == 1) break;
        PTE upd = s;
        upd.hardware.accessed = 1;
        if ((ULONG64)InterlockedCompareExchange64((LONG64*)pte,
            *(LONG64*)&upd,
            (LONG64)old) == old) break;
    }
    return;
}
