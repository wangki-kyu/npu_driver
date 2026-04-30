#include "Driver.h"
#include "Memory.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, ApexPageTableInit)
#pragma alloc_text(PAGE, ApexPageTableMap)
#pragma alloc_text(PAGE, ApexPageTableUnmap)
#pragma alloc_text(PAGE, ApexPageTableCleanup)
#endif

// Extended VA constants — must precede any function that references them.
// Mirror of definitions later in this file (kept here to satisfy C90 ordering).
#define APEX_EXTENDED_TABLE_BASE_INDEX  6144u
#define APEX_PAGES_PER_SUBTABLE         512u
#define APEX_EXTENDED_VA_BIT            (1ULL << 63)

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

    // extended_table = first extended entry index (libedgetpu: 6144).
    // PTE[0..6143] = simple (direct 4KB), PTE[6144..8191] = extended (2-level).
    apex_write_register(
        pDevContext->Bar2BaseAddress,
        APEX_REG_EXTENDED_TABLE,
        6144
    );

    // Zero-fill the entire hardware PTE array in BAR2 (0x50000 ~ 0x5FFFF, 8192 * 8 bytes).
    // Previous driver session may have left dirty PTEs with valid bit=1,
    // which causes inbound_page_fault when hardware walks unmapped VAs.
    {
        UINT32 j;
        for (j = 0; j < APEX_PAGE_TABLE_ENTRIES; j++) {
            apex_write_register(pDevContext->Bar2BaseAddress,
                                APEX_REG_PAGE_TABLE + (j * 8), 0);
        }
        DbgPrint("[%s] Hardware PTE array cleared (8192 entries)\n", __FUNCTION__);
    }

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
    _Inout_ UINT64 *DeviceAddress)
{
    PDEVICE_CONTEXT pDevContext;
    PMDL mdl = NULL;
    PPFN_NUMBER pfnArray = NULL;
    UINT32 pageCount;
    UINT32 startPte;
    UINT32 i;
    APEX_PTE *pte;
    PHYSICAL_ADDRESS physAddr;
    BOOLEAN isExtended;

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

    isExtended = (*DeviceAddress & APEX_EXTENDED_VA_BIT) != 0;

    // Caller-specified device VA must be page-aligned.
    if ((*DeviceAddress & (PAGE_SIZE - 1)) != 0) {
        DbgPrint("[%s] DeviceAddress 0x%llx not page-aligned\n", __FUNCTION__, *DeviceAddress);
        return STATUS_INVALID_PARAMETER;
    }

    pageCount = (UINT32)((Size + PAGE_SIZE - 1) / PAGE_SIZE);

    // Allocate MDL and lock user pages (common path for both simple/extended).
    mdl = IoAllocateMdl(UserBuffer, (ULONG)Size, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        DbgPrint("[%s] IoAllocateMdl failed\n", __FUNCTION__);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
        MmProbeAndLockPages(mdl, UserMode, IoWriteAccess);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DbgPrint("[%s] MmProbeAndLockPages failed\n", __FUNCTION__);
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    pfnArray = MmGetMdlPfnArray(mdl);

    if (isExtended) {
        // Extended VA path — populate second-level PT(s) via ApexExtMapBuffer.
        NTSTATUS extStatus = ApexExtMapBuffer(Device, *DeviceAddress, pfnArray, pageCount);
        if (!NT_SUCCESS(extStatus)) {
            DbgPrint("[%s] ApexExtMapBuffer failed VA=0x%llx pages=%u status=0x%x\n",
                __FUNCTION__, *DeviceAddress, pageCount, extStatus);
            MmUnlockPages(mdl);
            IoFreeMdl(mdl);
            return extStatus;
        }
        // Push existing LockedModelMdl into the extras list so it isn't leaked
        // when a second IOCTL_MAP_BUFFER comes in before PARAM_CACHE / UNMAP.
        if (pDevContext->LockedModelMdl != NULL) {
            if (pDevContext->ExtraLockedMdlCount < APEX_MAX_EXTRA_LOCKED_MDLS) {
                pDevContext->ExtraLockedMdls[pDevContext->ExtraLockedMdlCount++] =
                    pDevContext->LockedModelMdl;
                DbgPrint("[%s] EXT pushed prior LockedModelMdl=%p into extras[%u]\n",
                    __FUNCTION__, pDevContext->LockedModelMdl,
                    pDevContext->ExtraLockedMdlCount - 1);
            } else {
                DbgPrint("[%s] WARNING: extras full, force-unlock prior MDL=%p\n",
                    __FUNCTION__, pDevContext->LockedModelMdl);
                MmUnlockPages(pDevContext->LockedModelMdl);
                IoFreeMdl(pDevContext->LockedModelMdl);
            }
        }
        pDevContext->LockedModelMdl  = mdl;
        pDevContext->LockedModelSize = Size;
        DbgPrint("[%s] EXT mapped %u pages: UserBuffer=%p DeviceVA=0x%llx (ext)\n",
            __FUNCTION__, pageCount, UserBuffer, *DeviceAddress);
        DbgPrint("[%s] Saved MDL: %p, Size: 0x%llx\n", __FUNCTION__, mdl, Size);
        return STATUS_SUCCESS;
    }

    // Simple VA path (original).
    startPte = (UINT32)(*DeviceAddress >> PAGE_SHIFT);
    if (startPte + pageCount > pDevContext->PageTableSize) {
        DbgPrint("[%s] Mapping out of range: PTE[%lu..%lu] > %lu\n",
                 __FUNCTION__, startPte, startPte + pageCount - 1, pDevContext->PageTableSize);
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }

    pte = (APEX_PTE *)pDevContext->PageTableBase;

    WdfSpinLockAcquire(pDevContext->PageTableLock);

    DbgPrint("[MAP] HIB_ERROR before PTE loop = 0x%llx\n",
        apex_read_register(pDevContext->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS));

    for (i = 0; i < pageCount; i++) {
        physAddr.QuadPart = pfnArray[i] << PAGE_SHIFT;

        pte[startPte + i].address = physAddr.QuadPart & 0x0FFFFFFFFFFFFF;
        pte[startPte + i].status = PTE_INUSE;
        pte[startPte + i].dma_direction = 0;

        apex_write_register(
            pDevContext->Bar2BaseAddress,
            APEX_REG_PAGE_TABLE + ((startPte + i) * 8),
            pte[startPte + i].address | 0x1
        );
    }

    DbgPrint("[MAP] HIB_ERROR after PTE loop = 0x%llx\n",
        apex_read_register(pDevContext->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS));

    WdfSpinLockRelease(pDevContext->PageTableLock);

    // Same multi-MDL safety as the extended path.
    if (pDevContext->LockedModelMdl != NULL) {
        if (pDevContext->ExtraLockedMdlCount < APEX_MAX_EXTRA_LOCKED_MDLS) {
            pDevContext->ExtraLockedMdls[pDevContext->ExtraLockedMdlCount++] =
                pDevContext->LockedModelMdl;
            DbgPrint("[%s] SIMPLE pushed prior LockedModelMdl=%p into extras[%u]\n",
                __FUNCTION__, pDevContext->LockedModelMdl,
                pDevContext->ExtraLockedMdlCount - 1);
        } else {
            DbgPrint("[%s] WARNING: extras full, force-unlock prior MDL=%p\n",
                __FUNCTION__, pDevContext->LockedModelMdl);
            MmUnlockPages(pDevContext->LockedModelMdl);
            IoFreeMdl(pDevContext->LockedModelMdl);
        }
    }
    pDevContext->LockedModelMdl = mdl;
    pDevContext->LockedModelSize = Size;

    DbgPrint("[%s] Mapped %lu pages: UserBuffer=%p, DeviceAddr=0x%llx PTE[%lu..%lu]\n",
             __FUNCTION__, pageCount, UserBuffer, *DeviceAddress, startPte, startPte + pageCount - 1);
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

    if ((DeviceAddress & APEX_EXTENDED_VA_BIT) != 0) {
        // Extended VA: walk subtables and clear matching entries.
        UINT32 chipPteIdx     = APEX_EXTENDED_TABLE_BASE_INDEX +
                                (UINT32)((DeviceAddress >> 21) & 0x1FFF);
        UINT32 hostTableStart = (UINT32)((DeviceAddress >> 12) & 0x1FF);
        int sIdx;

        for (sIdx = 0; sIdx < APEX_MAX_EXT_SUBTABLES; sIdx++) {
            if (pDevContext->ExtSubtables[sIdx].Active &&
                pDevContext->ExtSubtables[sIdx].ChipPteIdx == chipPteIdx) {
                UINT64* slot = (UINT64*)pDevContext->ExtSubtables[sIdx].Kva;
                UINT32 j;
                UINT32 thisChunk = pageCount;
                if (hostTableStart + thisChunk > APEX_PAGES_PER_SUBTABLE) {
                    thisChunk = APEX_PAGES_PER_SUBTABLE - hostTableStart;
                }
                for (j = 0; j < thisChunk; j++) {
                    slot[hostTableStart + j] = 0;
                }
                DbgPrint("[%s] EXT unmapped %u pages: VA=0x%llx subtable[%d] hostIdx[%u..%u]\n",
                    __FUNCTION__, thisChunk, DeviceAddress, sIdx,
                    hostTableStart, hostTableStart + thisChunk - 1);
                return STATUS_SUCCESS;
            }
        }
        DbgPrint("[%s] EXT unmap: no subtable found for VA=0x%llx (chipPTE[%u])\n",
            __FUNCTION__, DeviceAddress, chipPteIdx);
        return STATUS_NOT_FOUND;
    }

    {
        UINT32 startPte = (UINT32)(DeviceAddress >> PAGE_SHIFT);

        if (startPte + pageCount > pDevContext->PageTableSize) {
            DbgPrint("[%s] Invalid range: PTE[%lu..%lu]\n",
                     __FUNCTION__, startPte, startPte + pageCount - 1);
            return STATUS_INVALID_PARAMETER;
        }

        pte = (APEX_PTE *)pDevContext->PageTableBase;

        WdfSpinLockAcquire(pDevContext->PageTableLock);

        for (i = 0; i < pageCount; i++) {
            pte[startPte + i].status = PTE_FREE;
            apex_write_register(
                pDevContext->Bar2BaseAddress,
                APEX_REG_PAGE_TABLE + ((startPte + i) * 8),
                0
            );
        }

        WdfSpinLockRelease(pDevContext->PageTableLock);

        DbgPrint("[%s] Unmapped %lu pages at device VA 0x%llx PTE[%lu..%lu]\n",
                 __FUNCTION__, pageCount, DeviceAddress, startPte, startPte + pageCount - 1);
    }

    return STATUS_SUCCESS;
}

VOID ApexPageTableCleanup(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDevContext;

    PAGED_CODE();

    pDevContext = DeviceGetContext(Device);

    // Drain any extra MDLs that piled up when multiple IOCTL_MAP_BUFFER calls
    // ran before an UNMAP / PARAM_CACHE handover (e.g. exe0 + PARAM bitstream
    // + INFER bitstream all mapped in one open).
    {
        UINT32 i;
        for (i = 0; i < pDevContext->ExtraLockedMdlCount; i++) {
            DbgPrint("[%s] Releasing extras[%u] MDL=%p\n",
                __FUNCTION__, i, pDevContext->ExtraLockedMdls[i]);
            MmUnlockPages(pDevContext->ExtraLockedMdls[i]);
            IoFreeMdl(pDevContext->ExtraLockedMdls[i]);
            pDevContext->ExtraLockedMdls[i] = NULL;
        }
        pDevContext->ExtraLockedMdlCount = 0;
    }

    // NOTE: LockedModelMdl should already be unlocked in IOCTL_UNMAP_BUFFER
    // Only unlock here if it wasn't properly unlocked (safety check)
    if (pDevContext->LockedModelMdl != NULL) {
        DbgPrint("[%s] WARNING: LockedModelMdl not unlocked earlier, unlocking now\n", __FUNCTION__);
        MmUnlockPages(pDevContext->LockedModelMdl);
        IoFreeMdl(pDevContext->LockedModelMdl);
        pDevContext->LockedModelMdl = NULL;
        pDevContext->LockedModelSize = 0;
    }

    // Defensive: if extended mapping or bounce was left active, clean it up.
    ApexExtUnmapBuffer(Device);
    ApexFreeOutputBounce(Device);

    if (pDevContext->PageTableBase != NULL) {
        ExFreePoolWithTag(pDevContext->PageTableBase, 'PTBL');
        pDevContext->PageTableBase = NULL;
    }

    DbgPrint("[%s] Page table cleaned up\n", __FUNCTION__);
}

// =========================================================================
// Extended VA mapping — see Memory.h header comment for the address layout.
//
// VA bit layout (extended):
//   [63] = 1 (extended flag)
//   [62:34] = reserved/zero
//   [33:21] = Extended PT Index → chip PTE register at (6144 + idx)
//   [20:12] = Host Table Index  → entry within the 2-level PT page
//   [11:0]  = Page Offset
// =========================================================================

// Internal helper: find or allocate a second-level PT for the given chip PTE index.
// Returns pointer to the slot in the pool (caller fills entries).
// On failure, returns NULL.
static int ApexExtFindOrAllocSubtable(
    _In_ PDEVICE_CONTEXT pDevContext,
    _In_ UINT32          chipPteIdx)
{
    int i;
    int freeSlot = -1;
    PHYSICAL_ADDRESS lowAddr, highAddr, noBoundary;
    PVOID secondLevelKva;
    PHYSICAL_ADDRESS secondLevelPa;

    // Try to find existing slot for this chipPteIdx.
    for (i = 0; i < APEX_MAX_EXT_SUBTABLES; i++) {
        if (pDevContext->ExtSubtables[i].Active &&
            pDevContext->ExtSubtables[i].ChipPteIdx == chipPteIdx) {
            return i;
        }
        if (!pDevContext->ExtSubtables[i].Active && freeSlot < 0) {
            freeSlot = i;
        }
    }

    if (freeSlot < 0) {
        DbgPrint("[ExtMap] Subtable pool exhausted (%u entries)\n", APEX_MAX_EXT_SUBTABLES);
        return -1;
    }

    // Allocate a new 4 KB second-level PT page < 4 GB, MmNonCached.
    lowAddr.QuadPart    = 0;
    highAddr.QuadPart   = 0xFFFFFFFFLL;
    noBoundary.QuadPart = 0;
    secondLevelKva = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowAddr, highAddr, noBoundary, MmNonCached);
    if (secondLevelKva == NULL) {
        DbgPrint("[ExtMap] Failed to allocate 2-level PT page (< 4 GB) for chipPteIdx=%u\n",
            chipPteIdx);
        return -1;
    }
    RtlZeroMemory(secondLevelKva, PAGE_SIZE);
    secondLevelPa = MmGetPhysicalAddress(secondLevelKva);

    // Write the chip PTE register pointing at this PT page.
    WdfSpinLockAcquire(pDevContext->PageTableLock);
    apex_write_register(
        pDevContext->Bar2BaseAddress,
        APEX_REG_PAGE_TABLE + (chipPteIdx * 8),
        (UINT64)secondLevelPa.QuadPart | 0x1ULL);
    WdfSpinLockRelease(pDevContext->PageTableLock);

    pDevContext->ExtSubtables[freeSlot].Kva        = secondLevelKva;
    pDevContext->ExtSubtables[freeSlot].Pa         = (UINT64)secondLevelPa.QuadPart;
    pDevContext->ExtSubtables[freeSlot].ChipPteIdx = chipPteIdx;
    pDevContext->ExtSubtables[freeSlot].Active     = TRUE;

    // Mirror to legacy single-slot fields for diagnostics.
    if (!pDevContext->ExtMappingActive) {
        pDevContext->ExtSecondLevelKva = secondLevelKva;
        pDevContext->ExtSecondLevelPa  = (UINT64)secondLevelPa.QuadPart;
        pDevContext->ExtChipPteIdx     = chipPteIdx;
        pDevContext->ExtMappingActive  = TRUE;
    }

    DbgPrint("[ExtMap] alloc subtable[%d] chipPTE[%u]=0x%llx kva=%p pa=0x%llx\n",
        freeSlot, chipPteIdx, (UINT64)secondLevelPa.QuadPart | 0x1ULL,
        secondLevelKva, (UINT64)secondLevelPa.QuadPart);
    return freeSlot;
}

NTSTATUS ApexExtMapBuffer(
    _In_ WDFDEVICE Device,
    _In_ UINT64 ExtDeviceVA,
    _In_ PPFN_NUMBER Pfns,
    _In_ UINT32 NumPages)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(Device);
    UINT64* slot;
    UINT32 i;
    UINT32 chipPteIdx;
    UINT32 hostTableStart;
    int subtableIdx;

    if (Pfns == NULL || NumPages == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((ExtDeviceVA & APEX_EXTENDED_VA_BIT) == 0) {
        DbgPrint("[ExtMap] ExtDeviceVA=0x%llx has no extended bit set\n", ExtDeviceVA);
        return STATUS_INVALID_PARAMETER;
    }
    if ((ExtDeviceVA & (PAGE_SIZE - 1)) != 0) {
        DbgPrint("[ExtMap] ExtDeviceVA=0x%llx not page-aligned\n", ExtDeviceVA);
        return STATUS_INVALID_PARAMETER;
    }

    // Decode chip PTE index and host-table starting offset from the VA.
    chipPteIdx     = APEX_EXTENDED_TABLE_BASE_INDEX + (UINT32)((ExtDeviceVA >> 21) & 0x1FFF);
    hostTableStart = (UINT32)((ExtDeviceVA >> 12) & 0x1FF);

    if (hostTableStart + NumPages > APEX_PAGES_PER_SUBTABLE) {
        // Buffer crosses a 2 MB extended region. Map across multiple subtables.
        UINT32 firstChunk = APEX_PAGES_PER_SUBTABLE - hostTableStart;
        NTSTATUS s1, s2;
        s1 = ApexExtMapBuffer(Device, ExtDeviceVA,
                              Pfns, firstChunk);
        if (!NT_SUCCESS(s1)) return s1;
        s2 = ApexExtMapBuffer(Device,
                              ExtDeviceVA + ((UINT64)firstChunk << PAGE_SHIFT),
                              Pfns + firstChunk,
                              NumPages - firstChunk);
        return s2;
    }

    // Find existing subtable for this chip PTE idx, or allocate new one.
    subtableIdx = ApexExtFindOrAllocSubtable(pDevContext, chipPteIdx);
    if (subtableIdx < 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Fill the 2-level PT entries.
    slot = (UINT64*)pDevContext->ExtSubtables[subtableIdx].Kva;
    for (i = 0; i < NumPages; i++) {
        UINT64 dataPa = ((UINT64)Pfns[i]) << PAGE_SHIFT;
        slot[hostTableStart + i] = dataPa | 0x1ULL;
    }

    DbgPrint("[ExtMap] VA=0x%llx -> subtable[%d] chipPTE[%u] hostIdx[%u..%u] %u pages\n",
        ExtDeviceVA, subtableIdx, chipPteIdx,
        hostTableStart, hostTableStart + NumPages - 1, NumPages);
    {
        UINT32 dumpN = (NumPages < 4) ? NumPages : 4;
        UINT32 j;
        for (j = 0; j < dumpN; j++) {
            DbgPrint("[ExtMap]   2-level PT[%u] = 0x%llx\n",
                hostTableStart + j, slot[hostTableStart + j]);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS ApexAllocOutputBounce(
    _In_  WDFDEVICE   Device,
    _In_  SIZE_T      Size,
    _Out_writes_(MaxPages) PPFN_NUMBER Pfns,
    _In_  UINT32      MaxPages,
    _Out_ UINT32     *NumPagesOut)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(Device);
    PHYSICAL_ADDRESS lowAddr;
    PHYSICAL_ADDRESS highAddr;
    PHYSICAL_ADDRESS noBoundary;
    PVOID bounceKva;
    PHYSICAL_ADDRESS bouncePa;
    SIZE_T alignedSize;
    UINT32 pageCount;
    UINT32 i;

    if (Size == 0 || Pfns == NULL || NumPagesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pDevContext->OutputBounceActive) {
        DbgPrint("[Bounce] WARNING: previous bounce still active — freeing first\n");
        ApexFreeOutputBounce(Device);
    }

    alignedSize = (Size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    pageCount   = (UINT32)(alignedSize >> PAGE_SHIFT);
    if (pageCount > MaxPages) {
        DbgPrint("[Bounce] caller PFN array too small: needed=%u max=%u\n",
            pageCount, MaxPages);
        return STATUS_INVALID_PARAMETER;
    }

    // < 4 GB, contiguous, MmNonCached so chip writes are CPU-visible without
    // explicit cache invalidate (we'll memcpy it into user space later).
    lowAddr.QuadPart    = 0;
    highAddr.QuadPart   = 0xFFFFFFFFLL;
    noBoundary.QuadPart = 0;
    bounceKva = MmAllocateContiguousMemorySpecifyCache(
        alignedSize, lowAddr, highAddr, noBoundary, MmNonCached);
    if (bounceKva == NULL) {
        DbgPrint("[Bounce] Failed to allocate %llu bytes < 4 GB\n",
            (UINT64)alignedSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Sentinel fill so post-INFER 0xCC means OUTFEED did not write.
    RtlFillMemory(bounceKva, alignedSize, 0xCC);

    bouncePa = MmGetPhysicalAddress(bounceKva);

    // Contiguous allocation: PFN sequence is base PFN + i.
    for (i = 0; i < pageCount; i++) {
        Pfns[i] = (PFN_NUMBER)(((UINT64)bouncePa.QuadPart >> PAGE_SHIFT) + i);
    }

    pDevContext->OutputBounceKva    = bounceKva;
    pDevContext->OutputBouncePa     = (UINT64)bouncePa.QuadPart;
    pDevContext->OutputBounceSize   = alignedSize;
    pDevContext->OutputBounceActive = TRUE;

    *NumPagesOut = pageCount;

    DbgPrint("[Bounce] alloc kva=%p pa=0x%llx size=0x%llx pages=%u (< 4 GB OK)\n",
        bounceKva, (UINT64)bouncePa.QuadPart, (UINT64)alignedSize, pageCount);
    return STATUS_SUCCESS;
}

VOID ApexFreeOutputBounce(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(Device);

    if (!pDevContext->OutputBounceActive) {
        return;
    }
    if (pDevContext->OutputBounceKva != NULL) {
        MmFreeContiguousMemory(pDevContext->OutputBounceKva);
    }
    DbgPrint("[Bounce] freed kva=%p pa=0x%llx size=0x%llx\n",
        pDevContext->OutputBounceKva,
        pDevContext->OutputBouncePa,
        (UINT64)pDevContext->OutputBounceSize);

    pDevContext->OutputBounceKva    = NULL;
    pDevContext->OutputBouncePa     = 0;
    pDevContext->OutputBounceSize   = 0;
    pDevContext->OutputBounceActive = FALSE;
}

VOID ApexExtUnmapBuffer(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(Device);
    int i;
    int freed = 0;

    for (i = 0; i < APEX_MAX_EXT_SUBTABLES; i++) {
        if (!pDevContext->ExtSubtables[i].Active) continue;

        // Clear the chip PTE register before freeing the host PT page.
        if (pDevContext->Bar2BaseAddress != NULL) {
            WdfSpinLockAcquire(pDevContext->PageTableLock);
            apex_write_register(
                pDevContext->Bar2BaseAddress,
                APEX_REG_PAGE_TABLE + (pDevContext->ExtSubtables[i].ChipPteIdx * 8),
                0);
            WdfSpinLockRelease(pDevContext->PageTableLock);
        }
        if (pDevContext->ExtSubtables[i].Kva != NULL) {
            MmFreeContiguousMemory(pDevContext->ExtSubtables[i].Kva);
        }
        DbgPrint("[ExtUnmap] subtable[%d] cleared chipPTE[%u] freed pa=0x%llx\n",
            i,
            pDevContext->ExtSubtables[i].ChipPteIdx,
            pDevContext->ExtSubtables[i].Pa);
        pDevContext->ExtSubtables[i].Kva        = NULL;
        pDevContext->ExtSubtables[i].Pa         = 0;
        pDevContext->ExtSubtables[i].ChipPteIdx = 0;
        pDevContext->ExtSubtables[i].Active     = FALSE;
        freed++;
    }

    pDevContext->ExtSecondLevelKva = NULL;
    pDevContext->ExtSecondLevelPa  = 0;
    pDevContext->ExtChipPteIdx     = 0;
    pDevContext->ExtMappingActive  = FALSE;

    if (freed > 0) {
        DbgPrint("[ExtUnmap] freed %d subtable(s)\n", freed);
    }
}
