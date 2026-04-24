#include "Driver.h"
#include "Memory.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, ApexPageTableInit)
#pragma alloc_text(PAGE, ApexPageTableMap)
#pragma alloc_text(PAGE, ApexPageTableUnmap)
#pragma alloc_text(PAGE, ApexPageTableCleanup)
#endif

// Page table entry structure
typedef struct {
    UINT64 address;      // Physical address (40-bit valid)
    UINT32 status;       // PTE_FREE(0) / PTE_INUSE(1)
    UINT32 dma_direction; // DMA direction
} APEX_PTE;

#define PTE_INUSE 1
#define PTE_FREE  0

NTSTATUS ApexPageTableInit(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDevContext;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T pageTableSize;
    WDF_OBJECT_ATTRIBUTES lockAttributes;

    PAGED_CODE();

    pDevContext = DeviceGetContext(Device);

    if (pDevContext->Bar2BaseAddress == NULL) {
        DbgPrint("[%s] BAR2 not mapped\n", __FUNCTION__);
        return STATUS_DEVICE_NOT_READY;
    }

    // Allocate page table memory (8192 entries * 8 bytes)
    pageTableSize = sizeof(APEX_PTE) * APEX_PAGE_TABLE_ENTRIES;

    #pragma warning(push)
    #pragma warning(disable:4996)
    pDevContext->PageTableBase = ExAllocatePoolWithTag(
        NonPagedPoolNx,
        pageTableSize,
        'PTBL'
    );
    #pragma warning(pop)

    if (pDevContext->PageTableBase == NULL) {
        DbgPrint("[%s] Failed to allocate page table\n", __FUNCTION__);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pDevContext->PageTableBase, pageTableSize);

    // Get physical address
    pDevContext->PageTablePhys = MmGetPhysicalAddress(pDevContext->PageTableBase);

    pDevContext->PageTableSize = APEX_PAGE_TABLE_ENTRIES;

    // Set page table size register
    apex_write_register(
        pDevContext->Bar2BaseAddress,
        APEX_REG_PAGE_TABLE_SIZE,
        (UINT64)APEX_PAGE_TABLE_ENTRIES
    );

    // Create spinlock for page table access
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = Device;

    status = WdfSpinLockCreate(&lockAttributes, &pDevContext->PageTableLock);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[%s] WdfSpinLockCreate failed: 0x%x\n", __FUNCTION__, status);
        ExFreePoolWithTag(pDevContext->PageTableBase, 'PTBL');
        pDevContext->PageTableBase = NULL;
        return status;
    }

    pDevContext->DeviceStatus = DEVICE_STATUS_ALIVE;

    DbgPrint("[%s] Page table initialized: VA=%p, PA=0x%llx, Size=%lu entries\n",
             __FUNCTION__,
             pDevContext->PageTableBase,
             pDevContext->PageTablePhys.QuadPart,
             pDevContext->PageTableSize);

    return STATUS_SUCCESS;
}

NTSTATUS ApexPageTableMap(
    _In_ WDFDEVICE Device,
    _In_ PVOID UserBuffer,
    _In_ SIZE_T Size,
    _Out_ UINT64 *DeviceAddress)
{
    PDEVICE_CONTEXT pDevContext;
    PMDL mdl = NULL;
    PPFN_NUMBER pfnArray = NULL;
    UINT32 pageCount;
    UINT32 i;
    APEX_PTE *pte;
    PHYSICAL_ADDRESS physAddr;

    PAGED_CODE();

    pDevContext = DeviceGetContext(Device);

    if (pDevContext->PageTableBase == NULL) {
        DbgPrint("[%s] Page table not initialized\n", __FUNCTION__);
        return STATUS_DEVICE_NOT_READY;
    }

    if (UserBuffer == NULL || Size == 0) {
        DbgPrint("[%s] Invalid parameters\n", __FUNCTION__);
        return STATUS_INVALID_PARAMETER;
    }

    // Calculate number of pages needed
    pageCount = (UINT32)((Size + PAGE_SIZE - 1) / PAGE_SIZE);

    if (pageCount > pDevContext->PageTableSize) {
        DbgPrint("[%s] Buffer too large: %lu pages > %lu available\n",
                 __FUNCTION__, pageCount, pDevContext->PageTableSize);
        return STATUS_INVALID_PARAMETER;
    }

    // Allocate MDL
    mdl = IoAllocateMdl(UserBuffer, (ULONG)Size, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        DbgPrint("[%s] IoAllocateMdl failed\n", __FUNCTION__);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Lock user pages
    __try {
        MmProbeAndLockPages(mdl, UserMode, IoWriteAccess);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DbgPrint("[%s] MmProbeAndLockPages failed\n", __FUNCTION__);
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }

    // Get physical page array
    pfnArray = MmGetMdlPfnArray(mdl);

    pte = (APEX_PTE *)pDevContext->PageTableBase;

    WdfSpinLockAcquire(pDevContext->PageTableLock);

    // Write page table entries to hardware
    for (i = 0; i < pageCount; i++) {
        physAddr.QuadPart = pfnArray[i] << PAGE_SHIFT;

        pte[i].address = physAddr.QuadPart & 0x0FFFFFFFFFFFFF;  // 40-bit address
        pte[i].status = PTE_INUSE;
        pte[i].dma_direction = 0;  // DMA_BIDIRECTIONAL

        // Write to BAR2 page table register
        apex_write_register(
            pDevContext->Bar2BaseAddress,
            APEX_REG_PAGE_TABLE + (i * 8),
            pte[i].address | 0x1  // Set PTE_INUSE bit
        );
    }

    // Enable translation
    apex_write_register(
        pDevContext->Bar2BaseAddress,
        APEX_REG_TRANSLATION_ENABLE,
        1
    );

    WdfSpinLockRelease(pDevContext->PageTableLock);

    // Device virtual address starts at 0
    *DeviceAddress = 0;

    // Keep MDL locked - will unlock in cleanup
    pDevContext->LockedModelMdl = mdl;
    pDevContext->LockedModelSize = Size;

    DbgPrint("[%s] Mapped %lu pages: UserBuffer=%p, DeviceAddr=0x%llx\n",
             __FUNCTION__, pageCount, UserBuffer, *DeviceAddress);
    DbgPrint("[%s] Pages locked and will remain locked during inference\n", __FUNCTION__);
    DbgPrint("[%s] Saved MDL: %p, Size: 0x%llx\n", __FUNCTION__, mdl, Size);

    return STATUS_SUCCESS;
}

NTSTATUS ApexPageTableUnmap(
    _In_ WDFDEVICE Device,
    _In_ UINT64 DeviceAddress,
    _In_ SIZE_T Size)
{
    PDEVICE_CONTEXT pDevContext;
    UINT32 pageCount;
    UINT32 i;
    APEX_PTE *pte;

    PAGED_CODE();

    pDevContext = DeviceGetContext(Device);

    if (pDevContext->PageTableBase == NULL) {
        DbgPrint("[%s] Page table not initialized\n", __FUNCTION__);
        return STATUS_DEVICE_NOT_READY;
    }

    pageCount = (UINT32)((Size + PAGE_SIZE - 1) / PAGE_SIZE);

    if (pageCount > pDevContext->PageTableSize) {
        DbgPrint("[%s] Invalid page count: %lu\n", __FUNCTION__, pageCount);
        return STATUS_INVALID_PARAMETER;
    }

    pte = (APEX_PTE *)pDevContext->PageTableBase;

    WdfSpinLockAcquire(pDevContext->PageTableLock);

    // Clear page table entries
    for (i = 0; i < pageCount; i++) {
        pte[i].status = PTE_FREE;

        apex_write_register(
            pDevContext->Bar2BaseAddress,
            APEX_REG_PAGE_TABLE + (i * 8),
            0
        );
    }

    WdfSpinLockRelease(pDevContext->PageTableLock);

    DbgPrint("[%s] Unmapped %lu pages at device address 0x%llx\n",
             __FUNCTION__, pageCount, DeviceAddress);

    return STATUS_SUCCESS;
}

VOID ApexPageTableCleanup(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDevContext;

    PAGED_CODE();

    pDevContext = DeviceGetContext(Device);

    // NOTE: LockedModelMdl should already be unlocked in IOCTL_UNMAP_BUFFER
    // Only unlock here if it wasn't properly unlocked (safety check)
    if (pDevContext->LockedModelMdl != NULL) {
        DbgPrint("[%s] WARNING: LockedModelMdl not unlocked earlier, unlocking now\n", __FUNCTION__);
        MmUnlockPages(pDevContext->LockedModelMdl);
        IoFreeMdl(pDevContext->LockedModelMdl);
        pDevContext->LockedModelMdl = NULL;
        pDevContext->LockedModelSize = 0;
    }

    if (pDevContext->PageTableBase != NULL) {
        ExFreePoolWithTag(pDevContext->PageTableBase, 'PTBL');
        pDevContext->PageTableBase = NULL;
    }

    DbgPrint("[%s] Page table cleaned up\n", __FUNCTION__);
}
