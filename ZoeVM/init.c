// init.c : One-time initialization and program setup for ZoeVM.
//
// Holds allocation of the PFN metadata table, PTE regions, disc/pagefile,
// events, the master init() entry point, and setup_program() (privilege
// acquisition + physical page / virtual address space allocation).

#include "vm.h"

// Malloc and memset vm to zero
PVOID
zero_malloc(
    size_t num_bytes
) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}

// Initialize pte lists
VOID
init_lists(
    VOID
) {
    InitializeList(&freeList_head);
    InitializeList(&activeList_head);
    InitializeList(&modifiedList_head);
    InitializeList(&standbyList_head);
}

// Commit only the page(s) of the reserved PFN table that hold this frame's slot.
// Idempotent: MEM_COMMIT on already-committed memory succeeds and is cheap.
static VOID
ensure_metadata_slot_is_committed(
    ULONG64 frame
) {
    PVOID slot_addr = (PVOID)&physical_slots[frame];

    // A slot can straddle a page boundary, so commit the whole span it covers.
    PVOID start = (PVOID)((ULONG_PTR)slot_addr & ~((ULONG_PTR)PAGE_SIZE - 1));
    PVOID last = (PVOID)(((ULONG_PTR)slot_addr + sizeof(pfn_metadata) - 1)
        & ~((ULONG_PTR)PAGE_SIZE - 1));
    SIZE_T span = (ULONG_PTR)last - (ULONG_PTR)start + PAGE_SIZE;

    if (VirtualAlloc(start, span, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        printf("ensure_metadata_slot_is_committed: commit failed for frame %llu, error %lu\n",
            frame, GetLastError());
        DebugBreak();
    }
}

// Initialize pfn metadata sparse array
VOID
init_pfn_metadata(
    ULONG_PTR physical_page_count,
    PULONG_PTR physical_page_numbers
) {
    // Find max existing frame number
    for (int i = 0; i < physical_page_count; i++) {
        max_frame_number = max(physical_page_numbers[i], max_frame_number);
    }

    // Reserve max_frame_number+1 amount of slots so that frame number can be index
    ULONG_PTR table_reserve_size = (max_frame_number + 1) * sizeof(pfn_metadata);
    physical_slots = VirtualAlloc(
        NULL,
        table_reserve_size,
        MEM_RESERVE,
        PAGE_READWRITE
    );
    if (physical_slots == NULL) {
        printf("init_pfn_metadata: failed to reserve and commit physical_slots. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Setup pfn metadata in physical_slots and add to free list
    for (ULONG_PTR i = 0; i < physical_page_count; i++) {
        ULONG64 frame = physical_page_numbers[i];

        ensure_metadata_slot_is_committed(frame);

        physical_slots[frame].frame_number = frame;
        physical_slots[frame].pte = NULL;
        physical_slots[frame].disc_index = INVALID_DISC_SLOT;
        physical_slots[frame].state.list_type = 0;
        physical_slots[frame].state.being_written = 0;
        physical_slots[frame].state.accessed = 0;
        physical_slots[frame].owner_thread_id = 0;
        physical_slots[frame].is_zero = 0;

        InitializeListHead(&physical_slots[frame].links);
        InitializeCriticalSectionAndSpinCount(&physical_slots[frame].lock, 0x00FFFFFF);
        InsertTailList(&freeList_head.entry, &physical_slots[frame].links);
        freeList_head.list_count++;
    }
    // Commit only the used parts of pfn_metadata sparse array
    // Report actual commit footprint
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR committed = 0;
    PCHAR addr = (PCHAR)physical_slots;
    PCHAR end = addr + table_reserve_size;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
        addr += mbi.RegionSize;
    }
    printf("PFN table: committed %llu MB of %llu MB reserved (%llu frames)\n",
        (ULONG64)committed / MB(1),
        (ULONG64)table_reserve_size / MB(1),
        (ULONG64)physical_page_count);
}

// Initialize disc
VOID
init_disc(VOID)
{
    disc_page_count = NUM_DISC_PAGES;
    disc = create_page_file(&disc_page_count);
    disc_free_stack = malloc(disc_page_count * sizeof(ULONG64));
    if (disc_free_stack == NULL) { DebugBreak(); }
    for (ULONG64 i = 0; i < disc_page_count; i++) {
        disc_free_stack[i] = i;          // every slot starts free
    }
    disc_stack_top = (LONG64)disc_page_count;   // all slots available and top == count
    InitializeCriticalSectionAndSpinCount(&disc_stack_lock, 0x00FFFFFF);
}

// Initialize global locks
VOID
init_global_locks(
    VOID
) {
    // None anymore
}

// Initialize pte locks
VOID
init_pte_regions(
    VOID
) {
    pte_regions = malloc(NUM_PTE_LOCKS * sizeof(PTE_REGION));
    if (pte_regions == NULL) {
        DebugBreak();
        return;
    }
    for (ULONG64 i = 0; i < NUM_PTE_LOCKS; i++) {
        InitializeCriticalSectionAndSpinCount(&pte_regions[i].lock, 0x00FFFFFF);
        pte_regions[i].active_page_count = 0;
        for (int age = 0; age < 8; age++) {
            pte_regions[i].age_counts[age] = 0;
        }
    }
}

// Initialize events
VOID
init_events(
    VOID
) {
    // Auto-reset events
    startAge_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    startTrim_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    diskReady_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    modifiedReady_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    // Manual-reset events
    redoFault_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL); // ZS implement event to shutdown all threads
}

VOID
init(
    ULONG_PTR physical_page_count,
    PULONG_PTR physical_page_numbers
) {
    init_lists();
    init_pfn_metadata(physical_page_count, physical_page_numbers);
    init_disc();
    init_events();
}

BOOL
GetPrivilege (
    VOID
)
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    //
    // This is Windows-specific code to acquire a privilege.
    // Understanding each line of it is not so important for
    // our efforts.
    //

    HANDLE hProcess;
    HANDLE Token;
    BOOL Result;

    //
    // Open the token.
    //

    hProcess = GetCurrentProcess ();

    Result = OpenProcessToken (hProcess,
                               TOKEN_ADJUST_PRIVILEGES,
                               &Token);

    if (Result == FALSE) {
        printf ("Cannot open process token.\n");
        return FALSE;
    }

    //
    // Enable the privilege.
    //

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    //
    // Get the LUID.
    //

    Result = LookupPrivilegeValue (NULL,
                                   SE_LOCK_MEMORY_NAME,
                                   &(Info.Privilege[0].Luid));

    if (Result == FALSE) {
        printf ("Cannot get privilege\n");
        return FALSE;
    }

    //
    // Adjust the privilege.
    //

    Result = AdjustTokenPrivileges (Token,
                                    FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0,
                                    NULL,
                                    NULL);

    //
    // Check the result.
    //

    if (Result == FALSE) {
        printf ("Cannot adjust token privileges %u\n", GetLastError ());
        return FALSE;
    }

    if (GetLastError () != ERROR_SUCCESS) {
        printf ("Cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy\n");
        return FALSE;
    }

    CloseHandle (Token);

    return TRUE;
}

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

HANDLE
CreateSharedMemorySection (
    VOID
    )
{
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Create an AWE section.  Later we deposit pages into it and/or
    // return them.
    //

    parameter.Type = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2 (INVALID_HANDLE_VALUE,
                                  NULL,
                                  SECTION_MAP_READ | SECTION_MAP_WRITE,
                                  PAGE_READWRITE,
                                  SEC_RESERVE,
                                  0,
                                  NULL,
                                  &parameter,
                                  1);

    return section;
}

#endif

BOOL
setup_program(
    VOID
)
{
    HANDLE physical_page_handle;

    //
    // Allocate the physical pages that we will be managing.
    //
    // First acquire privilege to do this since physical page control
    // is typically something the operating system reserves the sole
    // right to do.
    //

    BOOL privilege = GetPrivilege();

    if (privilege == FALSE) {
        printf("full_virtual_memory_test : could not get privilege\n");
        return FALSE;
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    physical_page_handle = CreateSharedMemorySection();

    if (physical_page_handle == NULL) {
        printf("CreateFileMapping2 failed, error %#x\n", GetLastError());
        return FALSE;
    }

#else

    physical_page_handle = GetCurrentProcess();

#endif
    init_global_locks();

    ULONG_PTR physical_page_count = NUMBER_OF_PHYSICAL_PAGES;
    physical_page_numbers = malloc(physical_page_count * sizeof(ULONG_PTR));


    if (physical_page_numbers == NULL) {
        printf("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return FALSE;
    }

    // Put PFNs in array so we can keep trac of which pages can be used and specifies the address
    BOOL allocated = AllocateUserPhysicalPages(physical_page_handle,
        &physical_page_count,
        physical_page_numbers);

    if (allocated == FALSE) {
        printf("full_virtual_memory_test : could not allocate physical pages\n");
        return FALSE;
    }

    // Create array to map PFNs in physical page count to their assigned VAs
    // track which VA each physical page is mapped to using second array (index, VA base + i*4k, or some other way to regenerate VA)
    // PULONG_PTR physical_page_to_virtual = malloc(physical_page_count * sizeof (ULONG_PTR));
    PULONG64 physical_page_to_virtual = malloc(physical_page_count * sizeof(ULONG64));


    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        printf("full_virtual_emory_test : allocated only %llu pages out of %u pages requested\n",
            physical_page_count,
            NUMBER_OF_PHYSICAL_PAGES);
    }

    init(physical_page_count, physical_page_numbers);


    // Set virtual address size based on physical and virtual page counts
    ULONG_PTR virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    // Round down to a PAGE_SIZE boundary
    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    // Set up page table for PTEs
    num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(num_ptes * sizeof(PTE));
    NUM_PTE_LOCKS = (num_ptes / 512) + 1;

    // Init PTE locks
    init_pte_regions();

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Allocate a MEM_PHYSICAL region that is "connected" to the AWE section
    // created above.
    //

    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;
    // mem_commit: want to use virtual memory later but cannot access it
    // mem_reserve: reserve virtual memory without allocating physical pages
    // mem_physical: map physical page into virtual address space
    VA_SPACE = VirtualAlloc2(NULL,
        NULL,
        virtual_address_size,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE,
        &parameter,
        1);

    temp_va_base = VirtualAlloc2(
        NULL,
        NULL,
        (SIZE_T)NUM_THREADS * WRITE_BATCH * PAGE_SIZE,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE,
        &parameter,      // same MemExtendedParameterUserPhysicalHandle as VA_SPACE
        1);
    if (temp_va_base == NULL) {
        printf("setup_program: could not reserve temp_va_base %x\n", GetLastError());
        return FALSE;
    }

#else
    VA_SPACE = VirtualAlloc(NULL,
        virtual_address_size,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE);

#endif

    if (VA_SPACE == NULL) {

        printf("full_virtual_memory_test : could not reserve memory %x\n",
            GetLastError());

        return FALSE;
    }
    return TRUE;
}
