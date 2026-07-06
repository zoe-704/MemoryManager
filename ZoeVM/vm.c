// DEBUGGER CHEATSHEET

// Left 8 digit number = start address, right = end address

// ? = equation/quick math
// lm = list module (lists all modules and where they are)
// ln = list near (give any address, and it will give which variable it belongs to)
// r = registers (shows all the cpu registers and the values within those registers and their variables/locations/instruction)
// kn = stack trace (current stack pointer (at top of VA space), return address (location of var it's returning to), and the function it is in (called call site))
// dv = dump variables (will show locations and values of all variables local to the function)
// bp = breakpoint (set breakpoint at function. ex: bp vmTest!main)
// g = go until breakpoint, completion, or crash
// u = unasemble (tells all the future instructions in your function)
// bl = lists all your breakpoints
// dd = Tries to show you the contents at an address
// .f+ go to next frame (who was the function before me?)
// dq = dump quad (dumps the values of 8 byte chunks at address specified)
// .logopen (opens text file with all the output of the debugger)
// .logclose
// gh = go ahead (degugger don't worry just continue)
// sxd av = (av = access violation) (tells debugger to stop breaking on this particular exception)*********\
// !vprot = (tells you if its legit and gives you the address for the memory allocation and the state ex. MEM_RESERVE, etc.)
// q = quit process
// bd = remove breakpoint (ex. bd 1 removes breakpoint 1)
// ?? var_name = gives value of that variable
// x = see list of globals
// ~*k dumps all threads running
// x ZoeVM!*page_count
// !critsec pte_lock

// Performance trace cheat sheet
// xperf -on base -stackwalk profile
// Then run your program
// xperf -stop -d trace1.etl
// wpa trace1.etl
// Once in the trace, click Trace and then Load Symbols

// GLOBAL LOCK ORDER:
//
// pte_lock
//      ↓
// pfn_lock
//      ↓
// activeList_lock
// modifiedList_lock
// standbyList_lock
// freeList_lock
//      ↓
// disc_lock

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

#define NUMBER_OF_PHYSICAL_PAGES   128

#define NUM_DISC_PAGES  (3 * NUMBER_OF_PHYSICAL_PAGES)

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)

#define MAX_TRIM_PAGES 64

#define ACTIVE_PAGE_TRIM_THRESHOLD (NUMBER_OF_PHYSICAL_PAGES * 3 / 4)  // trim when 75% full

#define PTES_PER_LOCK 512

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

typedef struct _LIST_HEAD_ENTRY {
    // ZS todo flink, blink, lock, counter
} LIST_HEAD_ENTRY;
#endif

ULONG_PTR chunks;

// HW
// ZS malloc space for locks (40 bytes per PTE & divide by 512)
// access via index &region[index] then just wait if collision (could be taking faults)
// 
// lock on each age list
// lock on free list (or muliple), standby list, etc.
// disc counter


ULONG64 NUM_PTE_LOCKS = 0;
CRITICAL_SECTION* pte_locks;
//CRITICAL_SECTION pte_lock;
CRITICAL_SECTION pfn_lock;
CRITICAL_SECTION mm_lock;
CRITICAL_SECTION disc_lock;

CRITICAL_SECTION freeList_lock;
CRITICAL_SECTION activeList_lock;
CRITICAL_SECTION modifiedList_lock;
CRITICAL_SECTION standbyList_lock;


BOOL trim_running = TRUE;
HANDLE startTrim_event; // fault thread -> trimmer: "pool low, start trimming"
HANDLE startDiskWrite_event; // trimmer -> disk writer: "modified list has pages"
HANDLE redoFault_event; // disk writer/trimmer -> fault threads: "pages available, retry"

unsigned va_access_count;
ULONG64 free_page_count = 0;
ULONG64 active_page_count = 0;
ULONG64 modified_page_count = 0;
ULONG64 standby_page_count = 0;


PBOOLEAN disc_metadata;
ULONG64* disc_free_stack;
ULONG64 disc_free_top = 0;
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
    ULONG64 accessed : 1;
    ULONG64 frame_number : 40;
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
PBOOLEAN written;
PVOID temp_va;
ULONG_PTR virtual_address_size_in_unsigned_chunks;
ULONG_PTR physical_page_numbers;

typedef struct _AGE_LIST_HEAD {
    LIST_ENTRY head;
    ULONG64 count;
} AGE_LIST_HEAD;
AGE_LIST_HEAD AgeLists[8];


// Struct for our PFNs
typedef struct _pfn_metadata {
    LIST_ENTRY links;        // free / active / modified / standby list
    LIST_ENTRY age_links;    // age bucket list (1–7), only valid when list_type == 01 (active)

    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index: MAX_DISC_PTE_BITS;

    ULONG64 list_type : 2;   // 00 free, 01 active, 10 modified, 11 standby
    ULONG64 age : 3;   // 1–7, 0 = not in any age bucket (free/modified/standby)
    ULONG64 being_written : 1;
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

CRITICAL_SECTION*
get_pte_lock(
    PPTE pte
) {
    ULONG64 pte_index = pte - page_table;
    ASSERT(pte >= page_table);
    ASSERT(pte_index / PTES_PER_LOCK < NUM_PTE_LOCKS);
    return &pte_locks[pte_index / PTES_PER_LOCK];
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
    if (IsListEmpty(&FreeList)) {
        return NULL;
    }
    PLIST_ENTRY entry = RemoveHeadList(&FreeList);  
    free_page_count--;
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
        printf("create_page_file: could not allocate disc_metadata and fake disc\n");
    }
    return p;
}

// Return first free disc slot to fill
ULONG64
get_disk_free_slots(
    VOID
)
{
    EnterCriticalSection(&disc_lock);

    if (disc_free_top == 0) {
        LeaveCriticalSection(&disc_lock);
        return (ULONG64)-1;
    }

    ULONG64 slot = disc_free_stack[--disc_free_top];
    ASSERT(disc_metadata[slot] == 0); // check if free
    disc_metadata[slot] = 1;

    LeaveCriticalSection(&disc_lock);
    return slot;
}

// Push back to stack and mark metadata as free
VOID
return_disk_free_slots(
    ULONG64 slot
) {
    ASSERT(slot < disc_page_count);

    EnterCriticalSection(&disc_lock);

    ASSERT(disc_metadata[slot] == 1);
    disc_metadata[slot] = 0;
    disc_free_stack[disc_free_top++] = slot;

    LeaveCriticalSection(&disc_lock);
}

// Insert pfn at tail of an age list
VOID 
AgeListInsert(
    pfn_metadata* pfn,
    ULONG64 age
) {
    ASSERT(age >= 1 && age <= 7);
    ASSERT(pfn->age == 0); // should not already be in an age list

    InsertTailList(&AgeLists[age].head, &pfn->age_links);
    pfn->age = age;
    AgeLists[age].count++;
}

// Remove pfn from whatever list it's in
VOID 
AgeListRemove(
    pfn_metadata* pfn
) {
    ASSERT(pfn->age >= 1 && pfn->age <= 7);

    RemoveEntryList(&pfn->age_links);
    AgeLists[pfn->age].count--;
    pfn->age = 0;
}

// Move pfn from whatever list it's in
VOID
AgeListMove(
    pfn_metadata* pfn,
    ULONG64 new_age
) {
    AgeListRemove(pfn);
    AgeListInsert(pfn, new_age);
}

// Age pfn from whatever list it's in
VOID
AgeListAge(
    pfn_metadata* pfn
) {
    if (pfn->age < 7) {
        AgeListMove(pfn, pfn->age + 1);
    }
}

BOOLEAN
PTE_WasAccessed(
    PPTE pte
) {
    return (BOOLEAN)pte->hardware.accessed;
}

VOID
PTE_ClearAccessedBit(
    PPTE pte
) {
    pte->hardware.accessed = 0;
}


VOID
AgeListTick(
    VOID
) {
    EnterCriticalSection(&pfn_lock);
    // Walk 6 down to 1 — avoids re-processing pages just moved up
    for (int age = 6; age >= 1; age--) {
        LIST_ENTRY* head = &AgeLists[age].head;
        LIST_ENTRY* entry = head->Flink;

        while (entry != head) {
            // Grab next before we potentially move this entry
            LIST_ENTRY* next = entry->Flink;

            pfn_metadata* pfn = CONTAINING_RECORD(entry, pfn_metadata, age_links);

            if (!PTE_WasAccessed(pfn->pte)) {
                AgeListAge(pfn);   // not touched — getting older
            }
            else {
                PTE_ClearAccessedBit(pfn->pte);  // touched — reset and leave in place
            }
            entry = next;
        }
    }
    LeaveCriticalSection(&pfn_lock);
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




pfn_metadata* unmap_pfns[MAX_TRIM_PAGES] = { NULL };

VOID
get_unmap_candidates(
    int* batch_count,
    INT batch_size,
    PULONG_PTR unmap_vas
) {
    EnterCriticalSection(&pfn_lock);
    for (int age = 7; age >= 1 && *batch_count < batch_size; age--) {
        LIST_ENTRY* head = &AgeLists[age].head;
        LIST_ENTRY* entry = head->Flink;
        while (entry != head && *batch_count < batch_size) {
            LIST_ENTRY* next = entry->Flink;
            pfn_metadata* pfn = CONTAINING_RECORD(entry, pfn_metadata, age_links);

            unmap_vas[*batch_count] = (PVOID)((ULONG_PTR)get_va_from_pte(pfn->pte) & ~(PAGE_SIZE - 1));
            unmap_pfns[*batch_count] = pfn;
            (*batch_count)++;

            // Pull off age list and move to modified
            AgeListRemove(pfn);
            RemoveEntryList(&pfn->links);
            active_page_count--;
            pfn->list_type = 2;
            InsertTailList(&ModifiedList, &pfn->links);
            modified_page_count++;
            entry = next;
        }
    }

    LeaveCriticalSection(&pfn_lock);    

    return;
}

VOID
write_to_disk(
    int* batch_count,
    PULONG_PTR unmap_vas
) {
    //EnterCriticalSection(&pte_lock);
    // Copy to disk before unmapping!!
    for (int i = 0; i < *batch_count; i++) {
        ULONG64 slot = get_disk_free_slots();
        if (slot == (ULONG64)-1) {
            *batch_count = i;
            break;
        }

        // 1. Unmap the victim VA — no thread can touch the physical page through it anymore
        if (MapUserPhysicalPages(unmap_vas[i], 1, NULL) == FALSE) {
            printf("get_unmap_candidates: failed to unmap victim VA\n");
            DebugBreak();
        }

        // 2. Map the physical frame to our private temp VA
        if (MapUserPhysicalPages(temp_va, 1, &unmap_pfns[i]->frame_number) == FALSE) {
            printf("get_unmap_candidates: failed to map to temp VA\n");
            DebugBreak();
        }

        // 3. Safe copy — physical page is only reachable through g_temp_va now
        memcpy((BYTE*)disc + slot * PAGE_SIZE, temp_va, PAGE_SIZE);

        // 4. Unmap temp VA so the frame is fully detached and reusable
        if (MapUserPhysicalPages(temp_va, 1, NULL) == FALSE) {
            printf("get_unmap_candidates: failed to unmap temp VA\n");
            DebugBreak();
        }

        unmap_pfns[i]->disc_index = slot;

        // 5. Update PTE to disc state
        PPTE evict_pte = unmap_pfns[i]->pte;
        CRITICAL_SECTION* region = get_pte_lock(evict_pte);
        EnterCriticalSection(region);
        evict_pte->disc.valid = 0;
        evict_pte->disc.transition = 0;
        evict_pte->disc.disc = 1;
        evict_pte->disc.disc_index = slot;
        LeaveCriticalSection(region);
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
    standby_page_count--;
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

VOID
init_lists(
    VOID
) {
    InitializeListHead(&FreeList);
    InitializeListHead(&ActiveList);
    InitializeListHead(&ModifiedList);
    InitializeListHead(&StandbyList);
}

// initialize age lists
VOID
init_AgeLists(
    VOID
) {
    for (int i = 1; i <= 7; i++) {
        InitializeListHead(&AgeLists[i].head);
        AgeLists[i].count = 0;
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
        free_page_count++;
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
    
    // Allocate stack and push slots onto it
    disc_free_stack = malloc(disc_page_count * sizeof(ULONG64));
    if (disc_free_stack == NULL) {
        printf("init_disc: failed to allocate disc_free_stack\n");
        DebugBreak();
    }

    InitializeCriticalSection(&disc_lock);

    // Push every slot index onto stack
    for (ULONG64 i = 0; i < disc_page_count; i++) {
        disc_free_stack[i] = i;
    }
    disc_free_top = disc_page_count; // all slots are free for now
}

VOID
init_temp_va(
    HANDLE physical_page_handle
) {
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;

    temp_va = VirtualAlloc2(
        NULL,
        NULL,
        PAGE_SIZE,
        MEM_RESERVE | MEM_PHYSICAL,
        PAGE_READWRITE,
        &parameter,
        1
    );
    if (temp_va == NULL) {
        printf("init_temp_va: failed to allocate temp VA, error %lu\n", GetLastError());
        DebugBreak();
    }
}

VOID
init_locks(
    VOID
) {
    // InitializeCriticalSection(&mm_lock);
    InitializeCriticalSectionAndSpinCount(&mm_lock, 0x00FFFFFF);
    InitializeCriticalSection(&disc_lock);
    //InitializeCriticalSection(&pte_lock);
    InitializeCriticalSection(&pfn_lock);

    InitializeCriticalSection(&freeList_lock);
    InitializeCriticalSection(&activeList_lock);
    InitializeCriticalSection(&modifiedList_lock);
    InitializeCriticalSection(&standbyList_lock);

    pte_locks = malloc(NUM_PTE_LOCKS * sizeof(CRITICAL_SECTION));
    for (int i = 0; i < NUM_PTE_LOCKS; i++) {
        InitializeCriticalSection(&pte_locks[i]);
    }
}

VOID
init_events(
    VOID
) {
    startTrim_event = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    startDiskWrite_event = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    redoFault_event = CreateEvent(NULL, TRUE, FALSE, NULL); // ZS todo decide fs on manual-reset???

    /* SetEvent(startTrim_event);
    SetEvent(startDiskWrite_event);
    SetEvent(redoFault_event); */
}

VOID
init(
    ULONG_PTR physical_page_count, 
    PULONG_PTR physical_page_numbers,
    HANDLE physical_page_handle
) {
    init_lists();
    init_AgeLists();
    init_pfn_metadata(physical_page_count, physical_page_numbers);
    init_disc();
    init_temp_va(physical_page_handle);
    init_locks();
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

    // ZS todo check if -1 too much
    // Set virtual address size based on physical and virtual page counts
    ULONG_PTR virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    // Round down to a PAGE_SIZE boundary
    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    // Set up page table for PTEs
    INT num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(num_ptes * sizeof(PTE));
    NUM_PTE_LOCKS = (num_ptes / 512) + 1;

    // Set up written array
    written = zero_malloc(virtual_address_size_in_unsigned_chunks * sizeof(BOOLEAN));

    // Initialize lists, pfn_metadata, disc
    init(physical_page_count, physical_page_numbers, physical_page_handle);


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


pfn_metadata*
get_free_pfn(
    VOID
) {
    EnterCriticalSection(&pfn_lock);
    pfn_metadata* new_pfn = NULL;

    // OPTION 1: check free list first
    if (!IsListEmpty(&FreeList)) {
        // Get pfn from free list
        PLIST_ENTRY free_entry = RemoveHeadList(&FreeList);
        free_page_count--;
        new_pfn = get_pfn_from_PListEntry(free_entry);
        LeaveCriticalSection(&pfn_lock);
        return new_pfn;
    }

    // OPTION 2: steal from standby (page is in memory but unmapped) 
    if (new_pfn == NULL && !IsListEmpty(&StandbyList)) {
        // Get pfn from standby list
        PLIST_ENTRY standby_entry = RemoveHeadList(&StandbyList);
        standby_page_count--;
        new_pfn = get_pfn_from_PListEntry(standby_entry);

        // Set old pte corresponding to the new_pfn to disc state
        PPTE old_pte = new_pfn->pte;
        if (old_pte != NULL) {
            old_pte->disc.valid = 0;
            old_pte->disc.transition = 0;
            old_pte->disc.disc = 1;
            old_pte->disc.disc_index = new_pfn->disc_index;
        }
        LeaveCriticalSection(&pfn_lock);
        return new_pfn;
    }

    // OPTION 3: trim active pages to modified and disk and then flush modified to standby
    if (new_pfn == NULL && !IsListEmpty(&StandbyList)) {
        new_pfn = get_pfn_from_standby();
        LeaveCriticalSection(&pfn_lock);
        return new_pfn;
    }

    LeaveCriticalSection(&pfn_lock);
    return NULL;
}

VOID
handle_soft_fault(
    PVOID arbitrary_va
) {
    PULONG_PTR page_aligned_va;

    //EnterCriticalSection(&pte_lock);
    PPTE pte = get_pte_from_va(arbitrary_va);
    CRITICAL_SECTION* region = get_pte_lock(pte);
    EnterCriticalSection(region);

    ULONG64 frame = pte->transition.frame_number;
    //LeaveCriticalSection(&pte_lock);
    LeaveCriticalSection(region);

    EnterCriticalSection(&pfn_lock);
    pfn_metadata* pfn = get_pfn_from_fn(frame);

    // Already rescued by another thread
    if (pfn->being_written == 0) {
        LeaveCriticalSection(&pfn_lock);
        return;
    }

    // Claim rescue under lock
    pfn->being_written = 0;
    LeaveCriticalSection(&pfn_lock);

    page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

    if (MapUserPhysicalPages(page_aligned_va, 1, &frame) == FALSE) {
        printf("handle_page_fault: rescue remap failed\n");
        DebugBreak();
    }

    // Reacquire lock to finish putting page back on active list
    //EnterCriticalSection(&pte_lock);
    EnterCriticalSection(region);
    EnterCriticalSection(&pfn_lock);

    pfn->list_type = 1;
    pfn->pte = pte;
    InsertTailList(&ActiveList, &pfn->links);
    active_page_count++;
    set_pte_valid(pte, frame);
    AgeListInsert(pfn, 1); // Add onto age lists

    LeaveCriticalSection(&pfn_lock);
    //LeaveCriticalSection(&pte_lock);
    LeaveCriticalSection(region);
    return;
}

VOID
handle_hard_fault(
    PVOID arbitrary_va
) {
    PULONG_PTR page_aligned_va;

    //EnterCriticalSection(&pte_lock);
    PPTE pte = get_pte_from_va(arbitrary_va);
    CRITICAL_SECTION* region = get_pte_lock(pte);
    EnterCriticalSection(region);

    EnterCriticalSection(&disc_lock);
    BOOL from_disc = pte->disc.disc;
    ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;
    LeaveCriticalSection(&disc_lock);

    // STEP 1: get a free pfn (free > standby > trim)
    pfn_metadata* new_pfn = NULL;
    int attempts = 0;
    while (new_pfn == NULL) {
        new_pfn = get_free_pfn();
        if (new_pfn != NULL) break;

        // Pool is empty so ask trimmer for pages 
        SetEvent(startTrim_event);

        // Drop pte_lock and block until someone says pages are ready
        //LeaveCriticalSection(&pte_lock);
        LeaveCriticalSection(region);

        WaitForSingleObject(redoFault_event, 20); // ZS todo finalize this
        ResetEvent(redoFault_event);
        //EnterCriticalSection(&pte_lock);
        EnterCriticalSection(region);

        // Another thread resolved this va
        if (pte->hardware.valid == 1) {
            //LeaveCriticalSection(&pte_lock);
            LeaveCriticalSection(region);
            return;
        }
        // Soft fault
        else if (pte->transition.transition == 1) {
            handle_soft_fault(arbitrary_va); // ZS todo can u enter the lock for disk between from_disc and leaving
        }
        // Hard fault
        else {
            if (++attempts > 1000) {
                printf("handle_hard_fault: pool exhausted after %d retries\n", attempts);
                DebugBreak();
                //LeaveCriticalSection(&pte_lock);
                LeaveCriticalSection(region);
                return;
            }
            continue; // Retry loop
        }
    }

    // STEP 2: if we got a page, map physical page to faulting VA
    if (new_pfn != NULL) {

        // PTE may have been resolved while pte_lock was dropped or on first-try path
        if (pte->hardware.valid == 1) {
            // Another thread has restored this page, so return pfn don't map/copy
            EnterCriticalSection(&pfn_lock);
            new_pfn->list_type = 0;
            InsertHeadList(&FreeList, &new_pfn->links);
            free_page_count++;
            LeaveCriticalSection(&pfn_lock);
            //LeaveCriticalSection(&pte_lock);
            LeaveCriticalSection(region);
            return;
        }

        // Re-read disc slot from the PTE under lock post-wait
        if (from_disc) {
            if (pte->disc.disc == 0) {
                // No longer a disc PTE, resolved by another path
                EnterCriticalSection(&pfn_lock);
                new_pfn->list_type = 0;
                InsertHeadList(&FreeList, &new_pfn->links);
                free_page_count++;
                LeaveCriticalSection(&pfn_lock);
                //LeaveCriticalSection(&pte_lock);
                LeaveCriticalSection(region);
                return;
            }
            old_disc_slot = pte->disc.disc_index; // Updated value
        }

        PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

        if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
            printf("full_virtual_memory_test : could not map VA %p to page %llX\n", arbitrary_va, &new_pfn->frame_number);
            return;
        }
        EnterCriticalSection(&pfn_lock);

        // STEP 3: restore from disk if needed
        if (from_disc) {
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
        new_pfn->being_written = 0;
        new_pfn->list_type = 1;
        new_pfn->pte = pte;
        InsertTailList(&ActiveList, &new_pfn->links);
        active_page_count++;
        AgeListInsert(new_pfn, 1); // new page starts at age 1

        set_pte_valid(pte, new_pfn->frame_number);
        LeaveCriticalSection(&pfn_lock);
        //LeaveCriticalSection(&pte_lock);
        LeaveCriticalSection(region);
    }
}

VOID
check_accuracy(
    VOID
) {
    printf("Checking the accuracy of stamped data\n");
    int num_corrupt_page = 0;

    for (int i = 0; i < virtual_address_size_in_unsigned_chunks; i++) {
        PULONG_PTR arbitrary_va = VA_SPACE + i;
        ULONG_PTR value;

        // If the page holds a value, it would be its own va
        // If the page does not hold a value, it is zeroed out
        ULONG_PTR expected_value = written[i] ? (ULONG_PTR)arbitrary_va : 0;
        BOOL checked_page_value = FALSE;

        while (!checked_page_value) {
            BOOL faulted = FALSE;
            __try {
                value = *arbitrary_va;
                checked_page_value = TRUE;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                faulted = TRUE;
            }

            // Do my if faulted logic flow
            if (faulted) {
                PPTE pte = get_pte_from_va(arbitrary_va);
                CRITICAL_SECTION* region = get_pte_lock(pte);
                EnterCriticalSection(region);
                // Already mapped, leave
                if (pte->hardware.valid == 1) {
                    //LeaveCriticalSection(&pte_lock);
                    LeaveCriticalSection(region);
                    continue;
                }
                else if (pte->transition.transition == 1) {
                    handle_soft_fault(arbitrary_va);
                }
                else {
                    handle_hard_fault(arbitrary_va);
                }
            }
        }

        // Check if the values are matching
        if (value != expected_value) {
            printf("Data accuracy failed at slot %llu where va %p: holds %llx, expected %llx\n",
                (unsigned long long)i, arbitrary_va,
                (unsigned long long)value, (unsigned long long)expected_value);
            num_corrupt_page++;
        }
    }

    // Print final statistics
    if (num_corrupt_page == 0) {
        printf("Check accuracy is all good.\n");
    }
    else {
        printf("Check accuracy failed %d corrput\n", num_corrupt_page);
        DebugBreak();
    }
}

DWORD WINAPI
page_fault_thread(
    PVOID parameter
)

{
    chunks = virtual_address_size_in_unsigned_chunks;
    unsigned i;
    // Each thread gets its own local rng state seeded from timestamp
    ULONG64 random_number;
    ULONG64 rng = __rdtsc();

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Page fault thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    BOOL page_faulted = FALSE;
    BOOL fault_resolution = TRUE;

    //
    // Now perform random accesses.
    //
    for (i = 1; i <= MB(1); i += 1) {
        va_access_count++;
        page_faulted = FALSE;

        // If arbitrary VA is empty or successfully stamped, generate next arbitrary VA
        if (fault_resolution == TRUE) {
            // xor random stuff
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            random_number = rng % chunks;
            random_number &= ~0x7;
        }
        PULONG_PTR arbitrary_va = VA_SPACE + random_number;

        // Some VAs can map to the same page and will not fault
        __try {
            *arbitrary_va = (ULONG_PTR)arbitrary_va;
            fault_resolution = TRUE;
            written[random_number] = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }

        PPTE pte = get_pte_from_va(arbitrary_va);
        CRITICAL_SECTION* region = get_pte_lock(pte);

        // Handle page fault 
        if (page_faulted) {
            // If page faulted, we want to redo this iteration to confirm successful mapping
            i--;
            fault_resolution = FALSE;

            // Check 1: another thread may have already resolved this fault
            if (pte->hardware.valid == 1) {
                continue;
            }

            // Check 2: another fault thread may have already rescued this
            else if (pte->transition.transition == 1) {
                handle_soft_fault(arbitrary_va);
            }

            // Check 3: we will hard fault if pte from disc or completely new
            else if (pte->hardware.valid == 0 && pte->transition.transition == 0)
            {
                handle_hard_fault(arbitrary_va);
            }

            // Check 4: something went wrong
            else {
                printf("handle_page_fault: Out of physical memory\n");
                DebugBreak();
            }
        }
        // Fault does not occur
        else {
            fault_resolution = TRUE;
            //EnterCriticalSection(&pte_lock);
            EnterCriticalSection(region);
            EnterCriticalSection(&pfn_lock);
            // ZS unprotected
            PPTE pte = get_pte_from_va(arbitrary_va);
            if (pte->hardware.valid == 1) {
                ULONG64 hardware_frame = pte->hardware.frame_number;
                pfn_metadata* active_pfn = get_pfn_from_fn(hardware_frame);
                if (active_pfn != NULL && active_pfn->pte != NULL && active_pfn->age != 0) {
                    if (active_pfn->age != 1) {
                        AgeListMove(active_pfn, 1);
                    }
                }
            }
            LeaveCriticalSection(&pfn_lock);
            //LeaveCriticalSection(&pte_lock);
            LeaveCriticalSection(region);
        }
    }
    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Thread execution time: %.2f ms\n", elapsed_ms);
    printf("Thread access iterations: %u\n", i);
    printf("==============================================\n\n");

    printf("page_fault_thread : finished accessing %u random virtual addresses\n", i);
    return 0;
}


// thread 3
DWORD WINAPI
trim_thread(
    PVOID parameter
)
{
    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Trim thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    while (trim_running) {
        WaitForSingleObject(startTrim_event, 20);
        if (!trim_running) break;

        //EnterCriticalSection(&pte_lock);
        EnterCriticalSection(&pfn_lock);

        // printf("TRIM: active=%llu threshold hit, staging\n", active_page_count);

        // Only trim if active list is getting full
        //if (active_page_count >= ACTIVE_PAGE_TRIM_THRESHOLD) {
        AgeListTick();

        // Only get candidates (disk thread will do the work)
        PULONG_PTR unmap_vas[MAX_TRIM_PAGES] = { NULL };
        INT count = 0;
        get_unmap_candidates(&count, MAX_TRIM_PAGES, unmap_vas);

        LeaveCriticalSection(&pfn_lock);
        //LeaveCriticalSection(&pte_lock);

        if (count > 0) {
            SetEvent(startDiskWrite_event);  // Wake up disk thread if there are trimmed pages
        }
    }

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Trim thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

    return 0;
}

// Move modified list to disk and push to standby
VOID
write_modified_list(
    VOID
)
{
    while (!IsListEmpty(&ModifiedList)) {
        //EnterCriticalSection(&pte_lock);
        EnterCriticalSection(&pfn_lock);

        if (IsListEmpty(&ModifiedList)) {
            LeaveCriticalSection(&pfn_lock);
            //LeaveCriticalSection(&pte_lock);
            break;
        }

        // Grab page off modified list and mark as in transition
        PLIST_ENTRY entry = RemoveHeadList(&ModifiedList);
        modified_page_count--;
        pfn_metadata* pfn = get_pfn_from_PListEntry(entry);
        PPTE pte = pfn->pte;
        CRITICAL_SECTION* region = get_pte_lock(pte);
        LeaveCriticalSection(&pfn_lock);

        // Indicate transition state so faulting thread can rescue this frame 
        // ZS todo function
        EnterCriticalSection(region);
        EnterCriticalSection(&pfn_lock);
        pfn->being_written = 1;
        pfn->pte->transition.valid = 0;
        pfn->pte->transition.transition = 1;
        pfn->pte->transition.frame_number = pfn->frame_number;
        PULONG_PTR victim_va = get_va_from_pte(pfn->pte);

        LeaveCriticalSection(&pfn_lock);
        //LeaveCriticalSection(&pte_lock);
        LeaveCriticalSection(region);

        // Get disk info outside of the lock
        ULONG64 slot = get_disk_free_slots();
        if (slot == (ULONG64)-1) {
            // If the disk is full undo transition and put it back on modified
            //EnterCriticalSection(&pte_lock);
            EnterCriticalSection(region);
            EnterCriticalSection(&pfn_lock);
            // Pfn has not been rescued yet
            if (pfn->being_written == 1) { 
                pfn->being_written = 0;
                pfn->pte->transition.transition = 0;
                pfn->list_type = 2;
                InsertHeadList(&ModifiedList, &pfn->links);
                modified_page_count++;
                SetEvent(startDiskWrite_event); // Set event for itself
            }
            LeaveCriticalSection(&pfn_lock);
            //LeaveCriticalSection(&pte_lock);
            LeaveCriticalSection(region);
            break;
        }

        // Unmap, copy via temp_va, update PTE (write_to_disk one page at a time)
        MapUserPhysicalPages(victim_va, 1, NULL);
        MapUserPhysicalPages(temp_va, 1, &pfn->frame_number);
        memcpy((BYTE*)disc + slot * PAGE_SIZE, temp_va, PAGE_SIZE);
        MapUserPhysicalPages(temp_va, 1, NULL);

        // Reacquire lock to update metadata and move to standby
        //EnterCriticalSection(&pte_lock);
        EnterCriticalSection(region);
        EnterCriticalSection(&pfn_lock);

        if (pfn->being_written == 0) {
            // Pfn was rescued during memcpy so disc entry is stale and throw it away
            LeaveCriticalSection(&pfn_lock);
            //LeaveCriticalSection(&pte_lock);
            LeaveCriticalSection(region);

            return_disk_free_slots(slot);
            continue;
        }

        // Not rescued so move data to disc and standby list
        pfn->being_written = 0;
        pfn->disc_index = slot;
        pfn->pte->disc.valid = 0;
        pfn->pte->disc.transition = 0;
        pfn->pte->disc.disc = 1;
        pfn->pte->disc.disc_index = slot;
        pfn->list_type = 3;
        pfn->pte = NULL;
        InsertTailList(&StandbyList, &pfn->links);
        standby_page_count++;

        LeaveCriticalSection(&pfn_lock);
        //LeaveCriticalSection(&pte_lock);
        LeaveCriticalSection(region);
    }

    // Signal fault threads that standby has pages
    SetEvent(redoFault_event);
}

// thread 4
DWORD WINAPI
disc_thread(
    PVOID parameter
)
{
    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Disc thread: starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);
    
    while (TRUE) {
        // Sleep until trim thread signals there's work
        WaitForSingleObject(startDiskWrite_event, 20);  // timeout so it checks trim_running
        if (!trim_running) break;
        write_modified_list();
        SetEvent(redoFault_event);
    }

    // Drain anything remaining before exit
    write_modified_list();

        // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Disk thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

    return 0;
}


VOID
full_virtual_memory_test(
    VOID
)
{
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    BOOL page_faulted;

    // Create timer
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    // Start timer
    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...\n");
    QueryPerformanceCounter(&start_time);

    if (!setup_program()) {
        printf("full_virtual_memory_test: setup failed\n");
        return;
    }

    // Pass virtual address size to both threads as parameter
    ULONG_PTR chunks = virtual_address_size_in_unsigned_chunks;

    // Create two page fault threads
    HANDLE threads[4];
    // Thread 1: faults
    printf("THREAD 1...\n");
    threads[0] = CreateThread(NULL, 0, page_fault_thread, NULL, 0, NULL);

    // Thread 2: faults
    printf("THREAD 2...\n");
    threads[1] = CreateThread(NULL, 0, page_fault_thread, NULL, 0, NULL);

    // Thread 3: trim helper")
    printf("THREAD 3...\n");
    threads[2] = CreateThread(NULL, 0, trim_thread, NULL, 0, NULL);

    // Thread 4: disc helper
    printf("THREAD 4...\n");
    threads[3] = CreateThread(NULL, 0, disc_thread, NULL, 0, NULL);

    if (threads[0] == NULL || threads[1] == NULL || threads[2] == NULL) {
        printf("Failed to create threads. Error: %lu\n", GetLastError());
        return;
    }

    // Wait for fault threads to finish
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);

    check_accuracy(); // single-threaded validation

    // Tell trim thread to stop and wait for it
    trim_running = FALSE;
    SetEvent(startTrim_event);       // wake trimmer
    SetEvent(startDiskWrite_event);  // wake disk

    WaitForSingleObject(threads[2], INFINITE);
    WaitForSingleObject(threads[3], INFINITE);


    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    CloseHandle(threads[2]);
    CloseHandle(threads[3]);
    CloseHandle(startDiskWrite_event);
    CloseHandle(startTrim_event);
    CloseHandle(redoFault_event);


    DeleteCriticalSection(&mm_lock); // ZS todo delete lock
    //DeleteCriticalSection(&pte_lock);
    DeleteCriticalSection(pte_locks);
    DeleteCriticalSection(&pfn_lock);
    DeleteCriticalSection(&disc_lock);
    DeleteCriticalSection(&freeList_lock);


    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("Disk thread: WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

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
    full_virtual_memory_test();
    return;
}

