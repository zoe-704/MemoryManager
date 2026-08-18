#include "vm.h"
#include "fault.h"

// Handles soft faults
// Rescues pages in transition state from standby list and disc back to active memory
BOOLEAN
handle_soft_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va)
{
    CRITICAL_SECTION* region = &region_struct->lock;  
    EnterCriticalSection(region);

    // Re-checks under lock
    if (pte->hardware.valid == 1) {
        LeaveCriticalSection(region);
        return TRUE;    // resolved by another thread
    }
    if (pte->transition.transition != 1) {
        LeaveCriticalSection(region);
        return FALSE;   // not a transition PTE to handle anymore
    }

    ULONG64 frame = pte->transition.frame_number;
    pfn_metadata* pfn = get_pfn_from_fn(frame);

    // Peek the list to pick a head
    ULONG64 lt1 = pfn->list_type;
    if (lt1 != LIST_MODIFIED && lt1 != LIST_STANDBY) {
        LeaveCriticalSection(region);
        return FALSE;
    }

    // Hold pfn->lock across entire removal
    EnterCriticalSection(&pfn->lock);

    // Re-read state under frame lock to confirm frame is still on modified/standby and belongs to pte
    // Frame couple be stolen off standby and repurposed to another VA
    ULONG64 lt2 = pfn->list_type;
    if ((lt2 != LIST_MODIFIED && lt2 != LIST_STANDBY) || pfn->being_written || pfn->pte != pte) {
        // Frame moved lists, mid-write, or repurposed before lock
        LeaveCriticalSection(&pfn->lock);
        LeaveCriticalSection(region);
        return FALSE;
    }
    PCONCURRENT_LIST_HEAD list_head = (lt2 == LIST_STANDBY) ? &standbyList_head : &modifiedList_head;

    // Set invalid disk slot if from standby list
    if (lt2 == LIST_STANDBY) {
        return_disk_free_slots(pfn->disc_index); // clears disc_slot_owner
        InterlockedIncrement64(&disk_debug[2]);
    }

    // Remove from the list it is on 
    RWListRemoveKnown(list_head, pfn);
    // Page must be fully off list (not mapped active and on standby)
    ASSERT(pfn->links.Flink == NULL && pfn->links.Blink == NULL);
    pfn->list_type = LIST_NONE;

    // Map to frame with pfn and region lock
    if (MapUserPhysicalPages(page_aligned_va, 1, &frame) == FALSE) {
        printf("handle_page_fault: rescue remap failed\n");
        DebugBreak();
    }

    // Finish putting page back on the active list from standby/modified having null links
    pfn->list_type = LIST_ACTIVE;
    pfn->pte = pte;
    // Edit PTE
    set_pte_valid(pte, frame, 1);
    region_add_active(region_struct, pfn, 0);   // add to active age list

    g_my_stats->soft_faults++;   // edit thread's own stats
    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(region);
    return TRUE;
}

// Handles hard faults
// PTE is either brand-nw or disc-backed (evicted)
// Need to acquire a physical frame, populate it (zero or read back from disc), and map it
BOOL
handle_hard_fault(PVOID arbitrary_va, PPTE pte, PPTE_REGION region_struct, PVOID page_aligned_va)
{
    CRITICAL_SECTION* region = &region_struct->lock;   // caller already derived region_struct

    // STEP 1: get a free pfn
    pfn_metadata* new_pfn = NULL;
    int attempts = 0;
    while (new_pfn == NULL) {
        EnterCriticalSection(region);

        // Page fault already resolved by someone else
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            return TRUE;
        }

        // Soft fault since someone else's rescue is in progress/going to start
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            BOOLEAN resolved = handle_soft_fault(arbitrary_va, pte, region_struct, page_aligned_va);
            if (resolved) return TRUE;
            continue;
        }

        // Hard fault path to get page (not valid or transition)
        LeaveCriticalSection(region);

        // Disc-backed fault overwrites frame from disc, so does not need page from zero list
        // Better to use zeroed frames for first touch zero PTEs
        BOOL from_disc_hint = pte->disc.disc;
        new_pfn = get_pfn_for_fault(from_disc_hint);
        if (new_pfn != NULL) break;

        // Empty pools so start both producers of pages to prevent stalls
        // trimmer (active->modified) and disc writer (modified->standby)
        SetEvent(startTrim_event);
        SetEvent(modifiedReady_event);
        ResetEvent(redoFault_event);

        new_pfn = get_pfn_for_fault(from_disc_hint);
        if (new_pfn != NULL) break;

        if (++attempts > 1000) {
            printf("handle_hard_fault: pool exhausted after %d retries\n", attempts);
            DebugBreak();
            return FALSE;
        }

        stage_ring_flush();

        g_my_stats->fault_waits++;   // fault thread stalled waiting for the page pool
        HANDLE waits[2] = { redoFault_event, shutdown_event };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) return FALSE;

    }

    PLIST_HEAD sh = &freeList_shards[free_shard_for_thread()];

    // STEP 2: if we got a page, map physical page to faulting VA
    EnterCriticalSection(region);
    if (new_pfn != NULL) {
        // PTE may have been resolved while pte_lock was dropped
        if (pte->hardware.valid == 1) {
            LeaveCriticalSection(region);
            // Another thread has restored this page so give frame back to free list
            EnterCriticalSection(&sh->list_lock);
            new_pfn->list_type = LIST_NONE;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            new_pfn->is_zero = 0;                       // free list holds garbage
            InsertHeadList(&sh->entry, &new_pfn->links);
            InterlockedIncrement64(&sh->list_count);
            LeaveCriticalSection(&sh->list_lock);
            SetEvent(startZero_event);
            return TRUE;
        }
        if (pte->transition.transition == 1) {
            LeaveCriticalSection(region);
            // Return frame like above
            EnterCriticalSection(&sh->list_lock);
            new_pfn->list_type = LIST_NONE;
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            new_pfn->is_zero = 0;                      
            InsertHeadList(&sh->entry, &new_pfn->links);
            InterlockedIncrement64(&sh->list_count);
            LeaveCriticalSection(&sh->list_lock);
            return FALSE; // retry from top
        }
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;

        // STEP 3: restore from disc via the private staging slot, before exposing at the real VA
        if (from_disc) {
            PULONG_PTR temp_va = stage_ring_map(new_pfn->frame_number);
            if (temp_va == NULL) {
                LeaveCriticalSection(region);
                return FALSE;
            }

            memcpy(temp_va, (char*)disc + (old_disc_slot * PAGE_SIZE), PAGE_SIZE);

            ULONG_PTR first = *(PULONG_PTR)temp_va;
            if (first != 0 && (first & ~(PAGE_SIZE - 1)) != (ULONG_PTR)page_aligned_va) {
                printf("hard fault: slot %llu holds data for VA %p, expected %p\n",
                    old_disc_slot, (PVOID)first, page_aligned_va);
                DebugBreak();
            }
            return_disk_free_slots(old_disc_slot);
            InterlockedIncrement64(&disk_debug[3]);
        }

        // STEP 4: expose at the real VA
        if (MapUserPhysicalPages(page_aligned_va, 1, &new_pfn->frame_number) == FALSE) {
            printf("could not map VA %p to frame %llX, err %lu\n",
                arbitrary_va, new_pfn->frame_number, GetLastError());
            LeaveCriticalSection(region);
            DebugBreak();
            return FALSE;
        }

        // Only zero a frame we did not just overwrite from disc
        if (!from_disc && !new_pfn->is_zero) {
            memset(page_aligned_va, 0, PAGE_SIZE);
            SetEvent(startZero_event);
        }

        // STEP 5: set to active list and valid state
        new_pfn->being_written = 0;
        new_pfn->list_type = LIST_ACTIVE;
        new_pfn->is_zero = 0;
        new_pfn->pte = pte;
        new_pfn->owner_thread_id = 0;

        set_pte_valid(pte, new_pfn->frame_number, 1);
        // Frame is just off free/zero/standby so add to active_age_lists[0]
        region_add_active(region_struct, new_pfn, 0);

        g_my_stats->hard_faults++;
        if (from_disc) g_my_stats->hard_disc++; else g_my_stats->hard_zero++;
        LeaveCriticalSection(region);
    }
    return TRUE;
}
