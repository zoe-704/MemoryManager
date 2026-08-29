// list.h : the list heads + list/RWList primitives

#pragma once
#include "vm.h"

// Page lists
// free/zero: coarse, single-lock lists
// modified/standby: concurrent SRW lists
extern LIST_HEAD            freeList_shards[NUM_FAULT_THREADS];
extern CONCURRENT_LIST_HEAD modifiedList_head;
extern CONCURRENT_LIST_HEAD standbyList_head;
extern LIST_HEAD            zeroList_head;

// Plain doubly-linked-list primitives
VOID        InitializeListHead(PLIST_ENTRY ListHead);
VOID        InitializeList(PLIST_HEAD Head);
VOID        InitializeConcurrentList(PCONCURRENT_LIST_HEAD Head);
BOOLEAN     IsListEmpty(PLIST_ENTRY ListHead);
VOID        InsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
VOID        InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN     RemoveEntryList(PLIST_ENTRY Entry);

// Concurrent list access 
VOID          RWListInsertTail(PCONCURRENT_LIST_HEAD L, pfn_metadata* node);
VOID          RWListRemoveKnown(PCONCURRENT_LIST_HEAD L, pfn_metadata* node);
pfn_metadata* RWListScanBegin(PCONCURRENT_LIST_HEAD L, RW_LIST_CURSOR* c, BOOLEAN forward);
pfn_metadata* RWListScanNext(RW_LIST_CURSOR* c);
pfn_metadata* RWListScanRemoveCurrent(RW_LIST_CURSOR* c);
VOID          RWListScanEnd(RW_LIST_CURSOR* c);
pfn_metadata* rwlist_scan_rescue_remove(RW_LIST_CURSOR* c);
VOID          check_concurrent_list(PCONCURRENT_LIST_HEAD L, ULONG64 expected_type, const char* name);
