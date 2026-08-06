#include "vm.h"

// Handles soft faults
// Rescues pages in transition state from standby list and disc back to active memory
BOOLEAN
handle_soft_fault(PVOID arbitrary_va)
{
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte);
    CRITICAL_SECTION* region = get_pte_lock(pte);
    EnterCriticalSection(region);

    // Re-check under lock since state may have changed before we got here
    if (pte->hardware.valid == 1) {
        LeaveCriticalSection(region);
        return TRUE;    // already resolved by another thread
    }
    if (pte->transition.transition != 1) {
        LeaveCriticalSection(region);
        return FALSE;   // not a transition PTE anymore to handle
    }

    ULONG64 frame = pte->transition.frame_number;
    pfn_metadata* pfn = get_pfn_from_fn(frame);

    // Peek the list to pick a head (re-checked authoritatively under the frame lock below)
    ULONG64 lt = pfn->state.list_type;
    if (lt != LIST_MODIFIED && lt != LIST_STANDBY) {
        LeaveCriticalSection(region);
        return FALSE;
    }

    // Hold pfn->lock across the entire removal (region -> pfn order preserved; the
    // list SRW + neighbor locks are taken inside RWListRemoveKnown, which only ever
    // TryEnters cross-node locks, so pfn-then-list here cannot deadlock).
    EnterCriticalSection(&pfn->lock);

    // Re-read list_type authoritatively under the frame lock.
    ULONG64 lt2 = pfn->state.list_type;
    if ((lt2 != LIST_MODIFIED && lt2 != LIST_STANDBY) || pfn->state.being_written) {
        // Frame moved lists / went mid-write between the peek and the lock: bail.
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

    // Remove from the list it is on (RWListRemoveKnown does the list_count decrement
    // and nulls both links, so a concurrent fault seeing a stale list_type no-ops
    // its own remove instead of corrupting the list).
    RWListRemoveKnown(list_head, pfn);
    // TRAP: activation is about to map this frame; it MUST be fully off the list now.
    // If the unlink silently failed we would activate a still-linked frame (the
    // "mapped-active + on standby" double-membership). Catch it here, under pfn->lock,
    // at the exact culprit -- far more informative than the downstream trimmer assert.
    ASSERT(pfn->links.Flink == NULL && pfn->links.Blink == NULL);
    // Mark it off any list. A concurrent fault peeks list_type without the lock;
    // LIST_NONE makes it bail early (or block on us then re-read and bail).
    pfn_state_set_list_type(pfn, LIST_NONE);

    // Align VA and map to frame (still holding pfn->lock and region)
    PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));
    if (MapUserPhysicalPages(page_aligned_va, 1, &frame) == FALSE) {
        printf("handle_page_fault: rescue remap failed\n");
        DebugBreak();
    }
#if DEBUG
    //validate_page_contents(page_aligned_va, "handle_soft_fault");
#endif

    // Finish putting page back on the active list (pfn->lock still held)
    pfn_state_set_list_type(pfn, LIST_ACTIVE);
    pfn->pte = pte; // ZSPFN
    // Edit PTE
    set_pte_valid(pte, frame, 1);
    region_struct->active_page_count++;
    region_struct->age_counts[0]++;

    InterlockedIncrement64(&soft_fault_count);
    LeaveCriticalSection(&pfn->lock);
    LeaveCriticalSection(region);
    return TRUE;
}
// ZS map neighbors in mapphysicalpages call to increase iefficiency significantly
// based on access/age bit set when faulted on and when not faulted, check which range of pte in and see how that influences your next moves

// Handles hard faults
// PTE is either brand-nw or disc-backed (evicted)
// Need to acquire a physical frame, populate it (zero or read back from disc), and map it
BOOL
handle_hard_fault(PVOID arbitrary_va)
{
    PPTE pte = get_pte_from_va(arbitrary_va);
    PPTE_REGION region_struct = get_pte_region(pte);
    CRITICAL_SECTION* region = get_pte_lock(pte);

    // STEP 1: get a free pfn (free > standby > trim)
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
            BOOLEAN resolved = handle_soft_fault(arbitrary_va);
            if (resolved) return TRUE;
            continue;
        }

        // Hard fault path since not valid nor transition so need to get a page
        LeaveCriticalSection(region);


        new_pfn = get_free_pfn();
        if (new_pfn != NULL) break;

        SetEvent(startTrim_event);
        ResetEvent(redoFault_event);

        new_pfn = get_free_pfn();
        if (new_pfn != NULL) break;

        // Pool is empty so ask trimmer for pages
        /*
        // Synchronize redoFault event with the SetEvent in modified writer
        EnterCriticalSection(&standbyList_head.list_lock);
        BOOL no_pages = pool_is_empty();
        if (no_pages) ResetEvent(redoFault_event);
        LeaveCriticalSection(&standbyList_head.list_lock);
        if (!no_pages) continue;*/

        if (++attempts > 1000) {
            printf("handle_hard_fault: pool exhausted after %d retries\n", attempts);
            DebugBreak();
            return FALSE;
        }

        stage_ring_flush();

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
            // ZS batch write this
            pfn_state_set_list_type(new_pfn, LIST_NONE);
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
            // ZS batch write this
            pfn_state_set_list_type(new_pfn, LIST_NONE);
            new_pfn->disc_index = INVALID_DISC_SLOT;
            new_pfn->owner_thread_id = 0;
            new_pfn->is_zero = 0;                       // free list holds garbage
            InsertHeadList(&sh->entry, &new_pfn->links);
            InterlockedIncrement64(&sh->list_count);
            LeaveCriticalSection(&sh->list_lock);
            return FALSE; // retry from top
        }
        BOOL from_disc = pte->disc.disc;
        ULONG64 old_disc_slot = from_disc ? pte->disc.disc_index : -1;
        PULONG_PTR page_aligned_va = (PVOID)((ULONG_PTR)arbitrary_va & ~(PAGE_SIZE - 1));

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
            /*
            if (MapUserPhysicalPages(temp_va, 1, NULL) == FALSE) {
                printf("could not unmap frame %llX from temp_va, err %lu\n",
                    new_pfn->frame_number, GetLastError());
                LeaveCriticalSection(region);
                DebugBreak();
                return FALSE;
            }*/

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

        // Only zero a frame we did NOT just overwrite from disc
        if (!from_disc && !new_pfn->is_zero) {
            memset(page_aligned_va, 0, PAGE_SIZE);
        }

#if DEBUG
        //validate_page_contents(page_aligned_va, "1695");
#endif

        // STEP 5: set to active state and update metadata
        // Insert onto active list and set as valid
        pfn_state_set_being_written(new_pfn, 0);
        pfn_state_set_list_type(new_pfn, LIST_ACTIVE);
        new_pfn->is_zero = 0;
        new_pfn->pte = pte;
        new_pfn->owner_thread_id = 0;

        set_pte_valid(pte, new_pfn->frame_number, 1);
        region_struct->active_page_count++;
        region_struct->age_counts[0]++;

        InterlockedIncrement64(&hard_fault_count);
        LeaveCriticalSection(region);
    }
    return TRUE;
}
