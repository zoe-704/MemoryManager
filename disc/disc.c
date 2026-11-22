
// Disc
// Create fake disc
PVOID
create_page_file(PULONG64 number_of_pages)
{
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
    return p;
}

// Lock-free disc-slot allocator with bitmap
// 
// One bit per slot (1 = used) and 64 in a LONG64 "row"
// Disc bitmap frees are atomic bit-clears
// Allocation by disc thread reserving slots into thread-local stash or claiming free bits 1 at atime
#define DISC_STASH_MAX (DISC_WRITE_BATCH * 2)
static __declspec(thread) ULONG64 disc_stash[DISC_STASH_MAX];
static __declspec(thread) int     disc_stash_count = 0;
static __declspec(thread) ULONG64 disc_scan_row = 0;   // resume the row scan here next refill

// Atomically claim one free slot's bit (TRUE if successful)
static BOOLEAN
disc_set_slot(ULONG64 slot)
{
    LONG64 mask = 1LL << (slot & 63);
    LONG64 old = InterlockedOr64(&disc_bitmap[slot >> 6], mask);
    if ((old & mask) != 0) return FALSE;   // already used
    InterlockedDecrement64(&g_disc_free_count);
    return TRUE;
}

// Reserve up to `target` free slots off the bitmap into the thread-local stash
static VOID
disc_refill_stash(int target)
{
    ULONG64 rows = disc_bitmap_rows;
    for (ULONG64 i = 0; i < rows && disc_stash_count < target; i++) {
        ULONG64 row = disc_scan_row;
        disc_scan_row = (disc_scan_row + 1 < rows) ? disc_scan_row + 1 : 0;

        LONG64 snap = disc_bitmap[row];
        if ((unsigned __int64)snap == ~0ULL) continue;   // full row: skip

        ULONG64 base = row * 64ULL;

        // Whole row free, so claim all 64
        if (snap == 0 && disc_stash_count + 64 <= DISC_STASH_MAX) {
            if (InterlockedCompareExchange64(&disc_bitmap[row], ~0LL, 0LL) == 0LL) {
                for (int b = 0; b < 64; b++) disc_stash[disc_stash_count++] = base + (ULONG64)b;
                InterlockedAdd64(&g_disc_free_count, -64);
                continue;
            }
            snap = disc_bitmap[row]; // rescan bits if lost the row
        }

        // Partial row, so claim each free bit individually
        for (int b = 0; b < 64 && disc_stash_count < DISC_STASH_MAX; b++) {
            if ((snap & (1LL << b)) != 0) continue; // used
            if (disc_set_slot(base + (ULONG64)b)) {
                disc_stash[disc_stash_count++] = base + (ULONG64)b;
            }
        }
    }
}

// Reserve one disc slot
// Hot: pop the stash OR Cold: refill the stash
ULONG64
get_disk_free_slots(VOID)
{
    InterlockedIncrement64(&disk_debug[0]);

    if (disc_stash_count == 0) {
        disc_refill_stash(DISC_WRITE_BATCH);
        if (disc_stash_count == 0) return (ULONG64)-1;   // disc full
    }

    ULONG64 slot = disc_stash[--disc_stash_count];
    InterlockedIncrement64(&disk_debug[1]);
    ASSERT(slot < disc_page_count);
    return slot;
}

// Any thread can free a disc slot with atomic bit-clear
VOID
return_disk_free_slots(ULONG64 slot)
{
    ASSERT(slot < disc_page_count);
    LONG64 clear_mask = ~(1LL << (slot & 63));
    LONG64 old = InterlockedAnd64(&disc_bitmap[slot >> 6], clear_mask);
    ASSERT((old & ~clear_mask) == ~clear_mask);   // the bit must have been set (in use)
    InterlockedIncrement64(&g_disc_free_count);
}

// Count of free disc slots excluding stashed ones
static ULONG64
disc_free_pages(VOID)
{
    LONG64 f = InterlockedOr64(&g_disc_free_count, 0);
    if (f < 0) f = 0;
    return (ULONG64)f > disc_page_count ? disc_page_count : (ULONG64)f;
}
