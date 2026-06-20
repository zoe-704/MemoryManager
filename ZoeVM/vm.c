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

#define VIRTUAL_ADDRESS_SIZE        KB(16)

#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

//
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//

#define NUMBER_OF_PHYSICAL_PAGES    10

size_t NUM_PTEs;

#define NUM_DISC_PAGES  4

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)


// Struct for our PTEs
// valid
typedef struct {
    ULONG64 valid : 1;
    ULONG64 frame_number : 40;
    ULONG64 age : 4;
    ULONG64 reserved : 19;
} VALID_PTE, * PVALID_PTE;

// transition
typedef struct {
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 1
    ULONG64 frame_number : 40;
    ULONG64 reserved : 22;
} TRANSITION_PTE, * PTRANSITION_PTE;

// disc
typedef struct {
    ULONG64 valid : 1; // = 0
    ULONG64 transition : 1; // = 0
    ULONG64 disc : 1; // = 1
    ULONG64 disc_index : MAX_DISC_PTE_BITS;
    ULONG64 reserved : 64 - MAX_DISC_PTE_BITS - 3;
} DISC_PTE, * PDISC_PTE;

// SAME 8 BYTES DIFFERENT VIEWS
typedef struct {
    union {
        VALID_PTE hardware;
        TRANSITION_PTE transition;
        DISC_PTE disc;
        //ZERO_PTE zero;
    };
} PTE, * PPTE;

PPTE page_table;
PULONG_PTR VA_SPACE;

// struct for our PFNs
typedef struct {
    LIST_ENTRY links;
    PPTE pte;
    ULONG64 disc_index;
    BOOL isOccupied;
} pfn_metadata;

// represents our physical memory slots
pfn_metadata physical_slots[NUMBER_OF_PHYSICAL_PAGES] = { 0 };

// physical_slot[1]->links.???



LIST_ENTRY free_list;
LIST_ENTRY active_list;
LIST_ENTRY standby_list;

/*
the direction from listhead to first entry is forward
the direction from tail to second to last entry is backward
changing the flink and blink relationships to insert (flink and blink to new entry)
OR remove (flink and blink over old entry)
*/

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


PVOID
zero_malloc(size_t num_bytes) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}

PPTE 
get_pte_from_va(PULONG_PTR va) {
    ULONG64 index = ((ULONG64)va - (ULONG64)VA_SPACE) / PAGE_SIZE;
    return page_table + index;
}

PULONG_PTR
get_va_from_pte(PPTE pte) {
    ULONG64 pte_index = pte - page_table;
    return VA_SPACE + (pte_index * (PAGE_SIZE / sizeof(ULONG_PTR)));
}


VOID 
set_pte_valid(PPTE pte, ULONG64 frame_number) {
    pte->hardware.frame_number = frame_number;
    pte->hardware.valid = 1;
}

VOID
set_pte_invalid(PPTE pte) {
    pte->hardware.valid = 0;
}

VOID
increment_pte_age(PVALID_PTE pte) {
    if (pte->age < 15) {
        pte->age++;
    }
}
/*
list entry in pfn
plug the things to batch unmap into this the list to batch unmap
need array to pass into for the unmap 
*/

size_t
find_oldest_pages_for_eviction(
    PVOID* unmap_vas, // to fill with VAs to be unmapped
    size_t max_unmap_capacity
) {
    unsigned i = 0;
    unsigned j = 0;
    size_t unmap_count = 0;
    size_t target_age = 15;
    size_t next_oldest_age = -1;

    // select up to 50% of oldest
    // changed from while to for loop, want to loop through multiple times
    while (unmap_count < max_unmap_capacity && target_age >= 0) {
        next_oldest_age = -1;

        for (i = 0; i < NUMBER_OF_PHYSICAL_PAGES; i++) {
            if (unmap_count >= max_unmap_capacity) break;

            if (physical_slots[i].isOccupied) {
                PPTE pte = physical_slots[i].pte;

                if (pte->hardware.valid) {
                    if (pte->hardware.age >= target_age) {
                        unmap_vas[unmap_count] = (PVOID)get_va_from_pte(pte);
                        unmap_count++;
                    }
                    else {
                        if (pte->hardware.age > next_oldest_age) {
                            next_oldest_age = (int)pte->hardware.age;
                        }
                    }
                }
            }
        }

        if (next_oldest_age != -1) {
            target_age = next_oldest_age;
        } else {
            target_age = 0;
        }
    }
    return unmap_count;
}

////// DISK /////
PBOOLEAN disc_metadata;

// searching for first free available disk slot to be filled
ULONG64 
get_disk_free_slots() {
    ULONG64 target_disk_index = -1;
    for (int i = 0; i < NUM_DISC_PAGES; i++) {
        if (disc_metadata[i] == 0) {
            target_disk_index = i;
            disc_metadata[i] = 1;
            break;
        }
    }
    // disk is full
    if (target_disk_index == -1) {
        return -1; // no spots
    }
    return target_disk_index;

}

// turn a given disk slot from being active/used to free/available
VOID
return_disk_free_slots(ULONG64 target_disk_index) {
    if (target_disk_index >= NUM_DISC_PAGES || disc_metadata[target_disk_index] == 0) {
        DebugBreak();
    }
    disc_metadata[target_disk_index] = 0;
}

// Create fake disk
PVOID
create_page_file(PULONG64 number_of_pages) {
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

//////////


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


/*
// declare how much memory you want to allocate and let OS determine how to deal with retrieval
VOID
malloc_test (
    VOID
    )
{
    unsigned i;
    PULONG_PTR p;
    unsigned random_number;
    // malloc = memory allocation of virtual address space 
    p = malloc (VIRTUAL_ADDRESS_SIZE);

    if (p == NULL) {
        printf ("malloc_test : could not malloc memory\n");
        return;
    }

    for (i = 0; i < MB (1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //

        random_number = rand ();

        random_number %= VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS;

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        *(p + random_number) = (ULONG_PTR) p;
    }

    printf ("malloc_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    free (p);

    return;
}

// control what happens when a fault occurs
VOID
commit_at_fault_time_test (
    VOID
    )
{
    unsigned i;
    PULONG_PTR p;
    PULONG_PTR committed_va;
    unsigned random_number;
    BOOL page_faulted;
    // virtual allocation for different level of access and control
    // mem_reserve: reserve but no access
    p = VirtualAlloc (NULL,
                      VIRTUAL_ADDRESS_SIZE,
                      MEM_RESERVE,
                      PAGE_NOACCESS);

    if (p == NULL) {
        printf ("commit_at_fault_time_test : could not reserve memory\n");
        return;
    }

    for (i = 0; i < MB (1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //

        random_number = rand ();

        random_number %= VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS;

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;
        __try {

            *(p + random_number) = (ULONG_PTR) p;

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {

            //
            // Commit the virtual address now - if that succeeds then
            // we'll be able to access it from now on.
            //

            committed_va = p + random_number;
            // mem_commit: pay for the space and can access 
            committed_va = VirtualAlloc (committed_va,
                                         sizeof (ULONG_PTR),
                                         MEM_COMMIT,
                                         PAGE_READWRITE);

            if (committed_va == NULL) {
                printf ("commit_at_fault_time_test : could not commit memory\n");
                return;
            }

            //
            // No exception handler needed now since we are guaranteed
            // by virtue of our commit that the operating system will
            // honor our access.
            //

            *committed_va = (ULONG_PTR) committed_va;
        }
    }

    printf ("commit_at_fault_time_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (p, 0, MEM_RELEASE);

    return;
}

*/



VOID
full_virtual_memory_test(
    VOID
)
{
    unsigned i, j, k;
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    int pfn_index = -1;
    BOOL allocated;
    BOOL page_faulted;
    BOOL privilege;
    ULONG_PTR physical_page_count;
    PULONG_PTR physical_page_numbers;
    HANDLE physical_page_handle;
    ULONG_PTR virtual_address_size;
    ULONG_PTR virtual_address_size_in_unsigned_chunks;



    //
    // Allocate the physical pages that we will be managing.
    //
    // First acquire privilege to do this since physical page control
    // is typically something the operating system reserves the sole
    // right to do.
    //

    privilege = GetPrivilege();

    if (privilege == FALSE) {
        printf("full_virtual_memory_test : could not get privilege\n");
        return;
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    physical_page_handle = CreateSharedMemorySection();

    if (physical_page_handle == NULL) {
        printf("CreateFileMapping2 failed, error %#x\n", GetLastError());
        return;
    }

#else

    physical_page_handle = GetCurrentProcess();

#endif

    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;
    physical_page_numbers = malloc(physical_page_count * sizeof(ULONG_PTR));


    if (physical_page_numbers == NULL) {
        printf("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return;
    }

    // Put PFNs in array so we can keep trac of which pages can be used and specifies the address
    allocated = AllocateUserPhysicalPages(physical_page_handle,
        &physical_page_count,
        physical_page_numbers);

    if (allocated == FALSE) {
        printf("full_virtual_memory_test : could not allocate physical pages\n");
        return;
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

    //
    // Reserve a user address space region using the Windows kernel
    // AWE (address windowing extensions) APIs.
    //
    // This will let us connect physical pages of our choosing to
    // any given virtual address within our allocated region.
    //
    // We deliberately make this much larger than physical memory
    // to illustrate how we can manage the illusion.
    //


    ULONG64 disc_page_count = NUM_DISC_PAGES;
    PVOID disc = create_page_file(&disc_page_count);
    virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    //
    // Round down to a PAGE_SIZE boundary.
    //

    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks =
        virtual_address_size / sizeof(ULONG_PTR);

    NUM_PTEs = virtual_address_size / PAGE_SIZE;

    page_table = zero_malloc(NUM_PTEs * sizeof(PTE));


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

        return;
    }

    // variables for scatter unmapping
    int mapped_count = 0;                               // counter of number of pages to scatter unmap

    //
    // Now perform random accesses.
    //
    for (i = 0; i < MB(1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //

        random_number = rand() * rand() * rand();

        random_number %= virtual_address_size_in_unsigned_chunks;

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;

        //
        // Ensure the write to the arbitrary virtual address doesn't
        // straddle a PAGE_SIZE boundary just to keep things simple for
        // now.
        //

        random_number &= ~0x7;

        arbitrary_va = VA_SPACE + random_number;

        // !! some VAs can map to the same page and will not fault
        __try {

            *arbitrary_va = (ULONG_PTR)arbitrary_va;

        }
        __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
            page_faulted = TRUE;
        }


        if (page_faulted) {

            //
            // Connect the virtual address now - if that succeeds then
            // we'll be able to access it from now on.
            //
            // THIS IS JUST REUSING THE SAME PHYSICAL PAGE OVER AND OVER !
            //
            // IT NEEDS TO BE REPLACED WITH A TRUE MEMORY MANAGEMENT
            // STATE MACHINE !
            //

            /*
            1. check if pool is full
                a. full --> scatter unmap oldest ??50%??
                b. convert the unmapped to disc state
            2. find new slot to map new page
            3. map new VA
            4. update page table and all tracking stuff
            5. increment age
            */

            // step 1 check if pool is full -- TODO can't you just compare mapped_count and number of physical pages??? BOOL slots_full = (mapped_count == NUMBER_OF_PHYSICAL_PAGES);
            BOOL slots_full = TRUE;
            
            for (j = 0; j < NUMBER_OF_PHYSICAL_PAGES; j++) {
                if (physical_slots[j].isOccupied == FALSE) {
                    slots_full = FALSE;
                    break;
                }
            }
            // step 1a full --> scatter unmap oldest 50%
            if (slots_full) {
                size_t unmap_max_capacity = NUMBER_OF_PHYSICAL_PAGES / 2 + 1;
                PULONG_PTR unmap_vas[NUMBER_OF_PHYSICAL_PAGES / 2 + 1] = { NULL };    // stores VAs to unmap

                size_t evicted_actual = find_oldest_pages_for_eviction(unmap_vas, unmap_max_capacity);


                // step 1b convert to disc state
                for (j = 0; j < evicted_actual; j++) {
                    PULONG_PTR evict_va = (PULONG_PTR)unmap_vas[j];
                    PPTE evict_pte = get_pte_from_va(evict_va);
                    for (k = 0; k < NUMBER_OF_PHYSICAL_PAGES; k++) {
                        if (physical_slots[k].pte == evict_pte) {
                            ULONG64 assigned_disc_slot = get_disk_free_slots(); // find first free disk slot
                            if (assigned_disc_slot != -1) {
                                // update pte
                                evict_pte->disc.valid = 0;
                                evict_pte->disc.transition = 0;
                                evict_pte->disc.disc = 1;
                                evict_pte->disc.disc_index = assigned_disc_slot;
                                evict_pte->disc.reserved = 0;

                                // copy to disc
                                memcpy((ULONG_PTR)disc + assigned_disc_slot * PAGE_SIZE, evict_va, PAGE_SIZE);

                                // free up tracked slots
                                physical_slots[k].isOccupied = FALSE;
                                physical_slots[k].pte = NULL;
                                mapped_count--;
                                break;
                            }
                            else {
                                evicted_actual = j;
                                break;
                            }
                             
                        }
                    }
                }

                // scatter unmap old pages
                if (MapUserPhysicalPagesScatter(unmap_vas, evicted_actual, NULL) == FALSE) {
                    printf("full_virtual_memory_test : could not scatter unmap\n");
                    return;
                }

            }


            // step 2 find new slot to map new page
            for (j = 0; j < NUMBER_OF_PHYSICAL_PAGES; j++) {
                if (!physical_slots[j].isOccupied) {
                    pfn_index = j;
                    break;
                }
            }

            if (pfn_index == -1) {
                printf("Memory pool is full.\n");
                return;
            }


            // step 3 map new VA
            PPTE pte = get_pte_from_va(arbitrary_va);

            // disk -> page and restore
            if (pte->disc.disc == 1 && pte->disc.valid == 0) {
                ULONG64 disc_slot = pte->disc.disc_index;
                if (MapUserPhysicalPages(arbitrary_va, 1, physical_page_numbers + pfn_index) == FALSE) {
                    printf("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, physical_page_numbers[pfn_index]);
                }

                // align va using mask: 15 = 1111 --> 0000
                PVOID aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1)); // TODO fix will fault when trying to align an unconnected arbitrary_va pre-mapping

                // copy from disk 
                memcpy(aligned_va, (ULONG_PTR)disc + disc_slot * PAGE_SIZE, PAGE_SIZE);
                return_disk_free_slots(disc_slot);

            }
            // new page
            else {
                if (MapUserPhysicalPages(arbitrary_va, 1, physical_page_numbers + pfn_index) == FALSE) {
                    printf("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, physical_page_numbers[pfn_index]);
                    return;
                }
            }


            // step 4 update page table and all tracking stuff
            pte->hardware.valid = 1;
            pte->hardware.frame_number = physical_page_numbers[pfn_index];
            pte->hardware.age = 0; // new page
            physical_slots[pfn_index].isOccupied = TRUE;
            physical_slots[pfn_index].pte = pte;
            mapped_count++;


            // step 5 update tracking stuff
            for (j = 0; j < NUMBER_OF_PHYSICAL_PAGES; j++) {
                if (physical_slots[j].isOccupied && physical_slots[j].pte != pte) {
                    if (physical_slots[j].pte->hardware.age < 15) {
                        physical_slots[j].pte->hardware.age++;
                    }
                }
            }


            /*
                if valid bit is set quick return
                else transition bit is set so rescue
                else on disk, read itt in on disk from this address
                otherwise completely empty, never been read before

                trim pte, take pfn and valid bit clear valid bit and set transition then put pfn in a local array or a local list w flink blink and put on modified list, with enough on modified list batch write to disk
                when write to disk, allocate disk addresses and save those in pfn, field in pfn for disk address when disk write finishes, move pages to
                page fau
            */



            /*
            // map immediately
            if (MapUserPhysicalPages(arbitrary_va, 1, physical_page_numbers + pfn_index) == FALSE) {
                printf("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, physical_page_numbers[pfn_index]);
                return;
            }

            // PTE state: valid from updated mapping
            set_pte_valid(pte, physical_page_numbers[pfn_index]);
            age_page(pte);

            *arbitrary_va = (ULONG_PTR)arbitrary_va;

            // stage for batch unmap later
            pending_vas[mapped_count] = (PVOID)arbitrary_va;
            mapped_count++;

            physical_page_to_virtual[pfn_index] = (ULONG64)arbitrary_va;

            // full so need to unmap VAs from physical pages
            if (mapped_count >= physical_page_count) {

                PVOID unmap_vas[NUMBER_OF_PHYSICAL_PAGES / 2];
                size_t max_unmap_capacity = NUMBER_OF_PHYSICAL_PAGES / 2;
                size_t pages_to_unmap = oldest_pages_for_eviction(pending_vas, unmap_vas, max_unmap_capacity);

                // scatter unmap old pages
                if (pages_to_unmap > 0) {
                    if (MapUserPhysicalPagesScatter(unmap_vas, pages_to_unmap, NULL) == FALSE)
                        printf("full_virtual_memory_test : could not scatter unmap\n");
                        return;
                    }


                for (j = 0; j < pages_to_unmap; j++) {
                    PULONG_PTR evicted_va = (PULONG_PTR)unmap_vas[j];
                    PPTE temp_pte = get_pte_from_va(evicted_va);

                    for (k = 0; k < physical_page_count; k++) {
                        if (physical_page_to_virtual[k] == (ULONG64)evicted_va) {
                            set_pte_transition(temp_pte, physical_page_numbers[k]);
                            break;
                        }
                    }
                }
                memset(pending_vas, 0, sizeof(pending_vas));
                memset(physical_page_to_virtual, 0, physical_page_count * sizeof(ULONG64));
                mapped_count = 0;

                /*

                // unmap all mapped VAs
                if (MapUserPhysicalPagesScatter(pending_vas, mapped_count - 1, NULL) == FALSE) {
                    printf("full_virtual_memory_test : could not scatter unmap\n");
                    return;
                }
                // PTE state: valid -> transition
                for (j = 0; j < mapped_count - 1; j++) {
                    va = (PULONG_PTR)pending_vas[j];
                    PPTE temp_pte = get_pte_from_va(va);
                    set_pte_transition(temp_pte, physical_page_numbers[j]);
                }
                mapped_count = 0;

            }

        }/*


            /*
            * helpful window debugging commands:
                ?       calculator mode
                lm
                ln      symbolic ouput of what's nearby
                r       shows all cpu registers that the program gets to use, the current instruction the debugger is at
                kn      stack trace: current stack pointer (where it is now), return address, call site
                dv      value of something w symbols
                bp      set breakpoint when one function is hit
                g       launches until breakpoint, end, or crash
                dd      retrieve contents at the address
                .f+     go up one level
                dq var  dump 8 byte chunks at the first address



            */

            /*

                     // physical page numbers = starting point
                     // number of pages = how many pages to look at
                     // NOTE *(p+1) goes to next address, not just to next byte (like index) ex. 1000, 1008, 1016

                     // change number of pages from 1 to 3 -->  breaks when try to access pages that don't exist
                     // map first three random addresses by +i (track which physical pages allocated)

                     // C
                     // number of ULONG_PTR per page
                     const size_t words_per_page = PAGE_SIZE / sizeof(ULONG_PTR);
                     // clear the low bits so random_number is a multiple of words_per_page
                     random_number &= ~(words_per_page - 1);
                     arbitrary_va = VA_SPACE + random_number;

                     //
                     // No exception handler needed now since we have connected
                     // the virtual address above to one of our physical pages
                     // so no subsequent fault can occur.
                     //

                     *arbitrary_va = (ULONG_PTR) arbitrary_va;


                     // way 1 -- normal array syntax
                     physical_page_to_virtual[pfn_index] = (ULONG64) arbitrary_va;

                     // way 2 -- pointer syntax
                     // *(physical_page_to_virtual + i) = (ULONG64) arbitrary_va;


                     //
                     // Unmap the virtual address translation we installed above
                     // now that we're done writing our value into it.
                     //

                     // if approaching limit, unmap myself bc random access
                     // random access currently
                     if (i >= physical_page_count - 1) {

                         if (MapUserPhysicalPages((PVOID) *(physical_page_to_virtual + pfn_index), 1, NULL) == FALSE) { // very expensive

                             printf("full_virtual_memory_test : could not unmap VA %p\n", arbitrary_va);

                             return;
                         }
                     }


                 }*/
        }
    }
    printf("full_virtual_memory_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree(VA_SPACE, 0, MEM_RELEASE);

    return;
    
}


VOID 
main(
    int argc,
    char** argv
)
{
    //
    // Test a simple malloc implementation - we call the operating
    // system to pay the up front cost to reserve and commit everything.
    //
    // Page faults will occur but the operating system will silently
    // handle them under the covers invisibly to us.
    //

    //malloc_test ();

    //
    // Test a slightly more complicated implementation - where we reserve
    // a big virtual address range up front, and only commit virtual
    // addresses as they get accessed.  This saves us from paying
    // commit costs for any portions we don't actually access.  But
    // the downside is what if we cannot commit it at the time of the
    // fault !
    //

    //commit_at_fault_time_test ();

    //
    // Test our very complicated usermode virtual implementation.
    // 
    // We will control the virtual and physical address space management
    // ourselves with the only two exceptions being that we will :
    //
    // 1. Ask the operating system for the physical pages we'll use to
    //    form our pool.
    //
    // 2. Ask the operating system to connect one of our virtual addresses
    //    to one of our physical pages (from our pool).
    //
    // We would do both of those operations ourselves but the operating
    // system (for security reasons) does not allow us to.
    //
    // But we will do all the heavy lifting of maintaining translation
    // tables, PFN data structures, management of physical pages,
    // virtual memory operations like handling page faults, materializing
    // mappings, freeing them, trimming them, writing them out to backing
    // store, bringing them back from backing store, protecting them, etc.
    //
    // This is where we can be as creative as we like, the sky's the limit !
    //

    full_virtual_memory_test();

    return;
}

