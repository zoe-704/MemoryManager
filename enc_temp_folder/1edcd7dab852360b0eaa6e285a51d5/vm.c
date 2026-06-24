#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

//
// This define enables code that lets us create multiple virtual address
// mappings to a single physical page.  We only/need want this if/when we
// start using reference counts to avoid holding locks while performing
// pagefile I/Os - because otherwise disallowing this makes it easier to
// detect and fix unintended failures to unmap virtual addresses properly.
//

#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 1

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

#define PAGE_SIZE                   4096

#define KB(x)                       ((x) * 1024)
#define MB(x)                       ((x) * 1024 * 1024)

//
// This is intentionally a power of two so we can use masking to stay
// within bounds.
//

// #define VIRTUAL_ADDRESS_SIZE        KB(16)

// #define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

//
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//

#define NUMBER_OF_PHYSICAL_PAGES   (MB(1024) / PAGE_SIZE)

#define NUM_DISC_PAGES  (3 * NUMBER_OF_PHYSICAL_PAGES)

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)

#define MAX_TRIM_PAGES 64

#define DEBUG 1

#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

#if 0
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY* Flink;
    struct _LIST_ENTRY* Blink;
} LIST_ENTRY, * PLIST_ENTRY;
#endif


// HW
// ZS malloc space for locks (40 bytes per PTE & divide by 512)
// access via index &region[index] then just wait if collision (could be taking faults)
// 
// TODO
// create another thread
// one thread runs initizliation code
// two threads to run page faults
// look at what breaks and what conflicts+collides to see where would need to add or adjust locks
CRITICAL_SECTION mm_lock;


PBOOLEAN disc_metadata;
PVOID disc;
ULONG64 disc_page_count;

// Functions for Linked Lists
VOID
InitializeListHead(
    PLIST_ENTRY ListHead
)
{
    ListHead->Flink = ListHead->Blink = ListHead;

    return;
}

BOOLEAN
IsListEmpty(
    PLIST_ENTRY ListHead
)
{
    return (BOOLEAN)(ListHead->Flink == ListHead);
}

VOID
InsertHeadList(
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Flink;

    //
    // Insert a new entry at the head.
    //

    Flink = ListHead->Flink;

    Entry->Flink = Flink;
    Entry->Blink = ListHead;

    Flink->Blink = Entry;

    ListHead->Flink = Entry;

    return;
}


VOID
InsertTailList(
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;

    //
    // Insert a new entry at the tail.
    //

    Blink = ListHead->Blink;

    Entry->Flink = ListHead;
    Entry->Blink = Blink;

    Blink->Flink = Entry;

    ListHead->Blink = Entry;

    return;
}

PLIST_ENTRY
RemoveHeadList(
    PLIST_ENTRY ListHead
)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    //
    // Remove the entry currently at the head of the list.
    //

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

BOOLEAN
RemoveEntryList(
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    //
    // Remove the caller's known entry.
    //

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    //
    // Return whether list is now empty.
    //

    return (BOOLEAN)(Flink == Blink);
}

LIST_ENTRY FreeList;
LIST_ENTRY ActiveList;
LIST_ENTRY ModifiedList;
LIST_ENTRY StandbyList;



// Struct for our PTEs
typedef struct _VALID_PTE{
    ULONG64 valid : 1;
    ULONG64 frame_number : 40;
    ULONG64 age : 1;
    ULONG64 reserved : 22;
} VALID_PTE, * PVALID_PTE;

typedef struct _TRANSITION_PTE{
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 1
    ULONG64 frame_number : 40;
    ULONG64 reserved : 22;
} TRANSITION_PTE, * PTRANSITION_PTE;

typedef struct _DISC_PTE{
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 0
    ULONG64 disc : 1; // = 1
    ULONG64 disc_index : MAX_DISC_PTE_BITS;
    ULONG64 reserved : 64 - MAX_DISC_PTE_BITS - 3;
} DISC_PTE, * PDISC_PTE;

// Same 8 bytes with different defined structures
typedef struct {
    union {
        VALID_PTE hardware;
        TRANSITION_PTE transition;
        DISC_PTE disc;
    };
} PTE, * PPTE;

PPTE page_table;
PULONG_PTR VA_SPACE;
ULONG_PTR virtual_address_size_in_unsigned_chunks;
ULONG_PTR physical_page_numbers;



// Struct for our PFNs
typedef struct _pfn_metadata {
    LIST_ENTRY links;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index: MAX_DISC_PTE_BITS;
    ULONG64 list_type : 2; // 00 free, 01 active, 10 modified, 11 standby
} pfn_metadata;

// Represents physical memory slots
// Used as an array of pointers mapping hardware frame number to metadata block
pfn_metadata* physical_slots = NULL;
ULONG64 max_frame_number = 0;



// Functions and helpers

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

// VA -> PTE
PPTE 
get_pte_from_va(
    PULONG_PTR va
) {
    ULONG64 index = ((ULONG64)va - (ULONG64)VA_SPACE) / PAGE_SIZE;
    return page_table + index;
}

// PTE -> VA
PULONG_PTR
get_va_from_pte(
    PPTE pte
) {
    ULONG64 pte_index = pte - page_table;
    PULONG_PTR page_va = VA_SPACE + (pte_index * (PAGE_SIZE / sizeof(ULONG_PTR)));
    return (PULONG_PTR)((ULONG_PTR)page_va & ~(PAGE_SIZE - 1)); // ZS align va within this function
}

// PListEntry -> metadata
pfn_metadata*
get_pfn_from_PListEntry(
    PLIST_ENTRY entry
) {
    if (entry == NULL) return NULL;
    return CONTAINING_RECORD(entry, pfn_metadata, links);
}


// FrameNumber -> PFN
pfn_metadata*
get_pfn_from_fn(
    ULONG64 fn
) {
    if (fn <= max_frame_number) {
        return &physical_slots[fn];
    }
    return NULL;
}

// Returns free page from FreeList
pfn_metadata*
get_free_page(
    VOID
)
{
    if (IsListEmpty(&FreeList)) return NULL;
    PLIST_ENTRY entry = RemoveHeadList(&FreeList);  
    return get_pfn_from_PListEntry(entry);
}
// Disc

// Create fake disk
PVOID
create_page_file(
    PULONG64 number_of_pages
) {
    PVOID p;
    if (*number_of_pages > MAX_DISC_SIZE) {
        *number_of_pages = MAX_DISC_SIZE;
    }
    ULONG64 num_bytes = *number_of_pages * PAGE_SIZE;
    p = malloc(num_bytes); // does not need to be memset bc will be overwritten 
    while (p == NULL) {
        num_bytes /= 2;
        p = malloc(num_bytes);
    }
    *number_of_pages = num_bytes / PAGE_SIZE;
    disc_metadata = malloc(*number_of_pages);
    memset(disc_metadata, 0, *number_of_pages); // set disc_metadata to all be 0
    if (disc_metadata == NULL) {
        printf("could not allocate disc_metadata and fake disc\n");
    }
    return p;
}

// Return first free disc slot to fill
ULONG64
get_disk_free_slots(
    VOID
)
{
    ULONG64 target_disk_index = -1; // Returns -1 if disc is full
    for (int i = 0; i < NUM_DISC_PAGES; i++) {
        if (disc_metadata[i] == 0) {
            target_disk_index = i;
            disc_metadata[i] = 1;
            break;
        }
    }
    return target_disk_index;

}

// Turn given disk slot from being active/used to free/available
VOID
return_disk_free_slots(
    ULONG64 target_disk_index
) {
    if (target_disk_index >= NUM_DISC_PAGES || disc_metadata[target_disk_index] == 0) {
        DebugBreak();
    }
    disc_metadata[target_disk_index] = 0;
}

// 
VOID
get_unmap_candidates_and_save_to_disc(
    int* batch_count,
    INT batch_size,
    PULONG_PTR unmap_vas
) {
    pfn_metadata* unmap_pfns[MAX_TRIM_PAGES] = { NULL };

    // Pass 1: walk ActiveList using Flink and collect pages with age > 4
    PLIST_ENTRY entry = ActiveList.Flink;
    while (entry != &ActiveList && *batch_count < batch_size) {
        pfn_metadata* pfn = (pfn_metadata*)entry;
        entry = entry->Flink; // advance entry before moving PFN off list

        if (pfn->pte->hardware.age > 0) {
            unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)get_va_from_pte(pfn->pte) & ~(PAGE_SIZE - 1));
            unmap_pfns[*batch_count] = pfn;
            (*batch_count)++;

            // Move to modified list
            RemoveEntryList(&pfn->links);
            pfn->list_type = 2;
            InsertTailList(&ModifiedList, &pfn->links);
        }
    }
    // Pass 2: top up from head of ActiveList until we reach MAX_TRIM_PAGES
    entry = ActiveList.Flink;
    while (entry != &ActiveList && *batch_count < batch_size) {
        pfn_metadata* pfn = (pfn_metadata*)entry;
        entry = entry->Flink; // advance entry before moving PFN off list

        if (pfn->list_type != 1) continue; // skip pages selected in pass 1

        if (pfn->pte->hardware.age <= 0) {
            unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)get_va_from_pte(pfn->pte) & ~(PAGE_SIZE - 1));
            unmap_pfns[*batch_count] = pfn;
            (*batch_count)++;

            RemoveEntryList(&pfn->links);   
            pfn->list_type = 2;             
            InsertTailList(&ModifiedList, &pfn->links);
        }
    }

    if (*batch_count == 0) return;

    // Copy to disk before unmapping!!
    for (int i = 0; i < *batch_count; i++) {
        ULONG64 slot = get_disk_free_slots();
        if (slot == (ULONG64)-1) {
            *batch_count = i;
            break;
        }
        memcpy((BYTE*)disc + slot * PAGE_SIZE, unmap_vas[i], PAGE_SIZE);
        unmap_pfns[i]->disc_index = slot;

        // Update PTE to disc state ZS make this a function
        PPTE evict_pte = unmap_pfns[i]->pte;
        evict_pte->disc.valid = 0;
        evict_pte->disc.transition = 0;
        evict_pte->disc.disc = 1;
        evict_pte->disc.disc_index = slot;

    }
}

VOID
set_pte_valid(
    PPTE pte, 
    ULONG64 frame_number
)
{
    pte->hardware.frame_number = frame_number;
    pte->hardware.valid = 1;
}

VOID
set_pte_invalid(
    PPTE pte
)
{
    pte->hardware.valid = 0;
}

VOID
increment_pte_age(
    PVALID_PTE pte
)
{
    if (pte->age < 15) {
        pte->age++;
    }
}



// Trim pages from unmap_candidates
VOID
trim_pages(
    VOID
)
{
    // Determine how many pages to trim
    INT batch_size = MAX_TRIM_PAGES;
    if (batch_size == 0) batch_size = 1;

    PULONG_PTR unmap_vas[MAX_TRIM_PAGES] = { NULL };
    INT count = 0;

    // Get candidates
    get_unmap_candidates_and_save_to_disc(&count, batch_size, unmap_vas);

    if (count > 0) {
        // Map candidates
        if (MapUserPhysicalPagesScatter(unmap_vas, count, NULL) == FALSE) {
            printf("get_pfn_and_trim_pages : scatter unmap failed\n");
            return;
        }

        // Move entries from modified to standby (already written to disk)
        while (!IsListEmpty(&ModifiedList)) {
            PLIST_ENTRY mod_entry = RemoveHeadList(&ModifiedList);
            pfn_metadata* transition_pfn = get_pfn_from_PListEntry(mod_entry);

            transition_pfn->list_type = 3; // standby list
            InsertTailList(&StandbyList, &transition_pfn->links);
        }
    }
}

// Rescue a free frame from standby list in disc state
pfn_metadata*
get_pfn_from_standby(
    VOID
)
{
    if (IsListEmpty(&StandbyList)) return NULL; 
    // Standby entry and its pfn
    PLIST_ENTRY standby_entry = RemoveHeadList(&StandbyList);
    pfn_metadata* new_pfn = get_pfn_from_PListEntry(standby_entry);

    if (new_pfn->pte != NULL) {
        // Set to disk state ZS function
        new_pfn->pte->disc.valid = 0;
        new_pfn->pte->disc.transition = 0;
        new_pfn->pte->disc.disc = 1;
        new_pfn->pte->disc.disc_index = new_pfn->disc_index;
        new_pfn->pte = NULL;
    }
    return new_pfn; // Rescued pfn
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
        MEM_RESERVE | MEM_COMMIT,     
        PAGE_READWRITE
    );
    if (physical_slots == NULL) {
        printf("init_pfn_metadata: failed to reserve and commit physical_slots. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Setup pfn metadata in physical_slots and add to free list
    for (int i = 0; i < physical_page_count; i++) {
        ULONG64 frame = physical_page_numbers[i];

        physical_slots[frame].frame_number = frame;
        physical_slots[frame].list_type = 0;
        InitializeListHead(&physical_slots[frame].links);

        InsertTailList(&FreeList, &physical_slots[frame].links);
    }
}

// Initialize disc
VOID
init_disc(
    VOID
)
{
    disc_page_count = NUM_DISC_PAGES;
    disc = create_page_file(&disc_page_count);
}

VOID
init(
    ULONG_PTR physical_page_count, 
    PULONG_PTR physical_page_numbers
) {
    InitializeCriticalSection(&mm_lock);

    InitializeListHead(&FreeList);
    InitializeListHead(&ActiveList);
    InitializeListHead(&ModifiedList);
    InitializeListHead(&StandbyList);

    init_pfn_metadata(physical_page_count, physical_page_numbers);
    init_disc();
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

        printf("full_virtual_memory_test : allocated only %llu pages out of %u pages requested\n",
            physical_page_count,
            NUMBER_OF_PHYSICAL_PAGES);
    }

    // Initialize lists, pfn_metadata, disc
    init(physical_page_count, physical_page_numbers);


    // Set virtual address size based on physical and virtual page counts
    ULONG_PTR virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    // Round down to a PAGE_SIZE boundary
    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    // Set up page table for PTEs
    INT num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(num_ptes * sizeof(PTE));


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


VOID
handle_page_fault(
    PVOID arbitrary_va
) 
{
    EnterCriticalSection(&mm_lock);

    PPTE pte = get_pte_from_va(arbitrary_va);

    // We will only fault if disc or completely new
    if (pte->hardware.valid == 0 && pte->transition.transition == 0)
    {
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;

        // STEP 1: get a free pfn (free > standby > trim)
        pfn_metadata* new_pfn = NULL;

        // 1A: check free list first
        if (!IsListEmpty(&FreeList)) {
            // Get pfn from free list
            PLIST_ENTRY free_entry = RemoveHeadList(&FreeList);
            new_pfn = get_pfn_from_PListEntry(free_entry);
        }
        // 1B: steal from stadnby (page is in memory but unmapped) 
        if (new_pfn == NULL && !IsListEmpty(&StandbyList)) {
            // Get pfn from standby list
            PLIST_ENTRY standby_entry = RemoveHeadList(&StandbyList);
            new_pfn = get_pfn_from_PListEntry(standby_entry);

            PPTE old_pte = new_pfn->pte;
            if (old_pte != NULL) {
                // Set to disc state ZS function
                old_pte->disc.valid = 0;
                old_pte->disc.transition = 0;
                old_pte->disc.disc = 1;
                old_pte->disc.disc_index = new_pfn->disc_index;
            }
        }
        // 1C: trim active pages to modified and disk and then flush modified to standby
        if (new_pfn == NULL) {
            trim_pages();
            if (!IsListEmpty(&StandbyList)) {
                new_pfn = get_pfn_from_standby();
            }
        }

        // STEP 2: if we got a page, map physical page to faulting VA
        if (new_pfn != NULL) {
            PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
            
            if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
                printf("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, &new_pfn->frame_number);
                InsertHeadList(&FreeList, &new_pfn->links);  // ZS — return pfn or it's lost

                return;
            }

            // STEP 3: restore from disk if needed
            if (from_disc) {
                if (old_disc_slot == (ULONG64)-1 || old_disc_slot >= NUM_DISC_PAGES) {
                    printf("handle_page_fault: invalid disc_index %llu\n", old_disc_slot);
                    DebugBreak();
                }

                // Get data from disc and copy to new page
                VOID* disc_address = (char*)disc + (old_disc_slot * PAGE_SIZE);
                memcpy(page_aligned_va, disc_address, PAGE_SIZE);
                return_disk_free_slots(old_disc_slot); // Free up slot
            }
            // STEP 4: otherwise clear it
            else {
                memset(page_aligned_va, 0, PAGE_SIZE);
            }
            // STEP 5: set to active state and update pte and pfn metadata
            new_pfn->list_type = 1;
            new_pfn->pte = pte;
            InsertTailList(&ActiveList, &new_pfn->links);

            set_pte_valid(pte, new_pfn->frame_number);
            pte->hardware.age = 0;
        }
    }
    else {
        printf("handle_page_fault: Out of physical memory\n");
        DebugBreak();
    }
    LeaveCriticalSection(&mm_lock); // release lock
}


VOID
full_virtual_memory_test(
    VOID
)
{
    unsigned i;
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    BOOL page_faulted;

    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    if (!setup_program()) return;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);


    // Pass virtual address size to both threads as parameter
    ULONG_PTR chunks = virtual_address_size_in_unsigned_chunks;

    // Create two page fault threads
    HANDLE threads[2];
    threads[0] = CreateThread(
        NULL,                   // default security
        0,                      // default stack size
        handle_page_fault,      // function to run
        &chunks,                // parameter passed to function
        0,                      // start immediately
        NULL                    // don't need thread ID
    );
    threads[1] = CreateThread(
        NULL,
        0,
        handle_page_fault,
        &chunks,
        0,
        NULL
    );

    if (threads[0] == NULL || threads[1] == NULL) {
        printf("Failed to create threads. Error: %lu\n", GetLastError());
        return;
    }


    //
    // Now perform random accesses.
    //
    for (i = 0; i < KB(1); i += 1) {
        random_number = rand() * rand() * rand();
        random_number %= chunks;
        random_number &= ~0x7;

        arbitrary_va = VA_SPACE + random_number;

        page_faulted = FALSE;


        // Some VAs can map to the same page and will not fault
        __try {
            *arbitrary_va = (ULONG_PTR)arbitrary_va;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }


        // Handle page fault
        if (page_faulted) {
            handle_page_fault(arbitrary_va);
        }
        // Fault does not occur
        else {
            PPTE pte = get_pte_from_va(arbitrary_va);
            ULONG64 hardware_frame = pte->hardware.frame_number;
            pfn_metadata* active_pfn = get_pfn_from_fn(hardware_frame);
            if (active_pfn != NULL && active_pfn->pte != NULL) {
                active_pfn->pte->hardware.age = 1;
            }
        }
    }
        
    DeleteCriticalSection(&mm_lock); // delete lock

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("Total access iterations: %u\n", i);
    printf("==============================================\n\n");

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //
    printf("full_virtual_memory_test : finished accessing %u random virtual addresses\n", i);

    VirtualFree(VA_SPACE, 0, MEM_RELEASE);
    return;
    
}


VOID 
main(
    int argc,
    char** argv
)
{
    full_virtual_memory_test();
    return;
}

