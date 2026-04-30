#pragma once
#include <ntdef.h>
#include <wdf.h>

// Page table management functions
NTSTATUS ApexPageTableInit(_In_ WDFDEVICE Device);

NTSTATUS ApexPageTableMap(
    _In_ WDFDEVICE Device,
    _In_ PVOID UserBuffer,
    _In_ SIZE_T Size,
    _Inout_ UINT64 *DeviceAddress  // in: requested device VA (page-aligned); 0 = legacy "VA 0"
);

NTSTATUS ApexPageTableUnmap(
    _In_ WDFDEVICE Device,
    _In_ UINT64 DeviceAddress,
    _In_ SIZE_T Size
);

VOID ApexPageTableCleanup(_In_ WDFDEVICE Device);

// =========================================================================
// Extended-VA mapping helpers.
//
// When a buffer's device VA has bit 63 set (extended), the chip MMU walker
// expects the PTE register at index (6144 + ((VA>>21)&0x1FFF)) to hold the
// PA of a 4 KB second-level page table (host-resident, 512 entries × 8 B).
// Each entry in that 2-level PT covers one 4 KB data page.
//
// ApexExtMapBuffer:
//   - ExtDeviceVA must have bit 63 set, page-aligned
//   - Up to 512 pages × 4 KB = 2 MB per extended region (per chip PTE entry).
//     Caller responsible for staying within one region (single chip PTE).
//   - Pfns is an array of host PFNs (page frame numbers, PA >> 12).
//   - Returns STATUS_SUCCESS and stores state in DEVICE_CONTEXT
//     (ExtSecondLevelKva, ExtSecondLevelPa, ExtChipPteIdx, ExtMappingActive).
//
// ApexExtUnmapBuffer:
//   - Clears the chip PTE register and frees the second-level PT page.
//   - Idempotent: returns immediately if ExtMappingActive == FALSE.
// =========================================================================
NTSTATUS ApexExtMapBuffer(
    _In_ WDFDEVICE Device,
    _In_ UINT64 ExtDeviceVA,
    _In_ PPFN_NUMBER Pfns,
    _In_ UINT32 NumPages
);

VOID ApexExtUnmapBuffer(_In_ WDFDEVICE Device);

// =========================================================================
// Output bounce buffer helpers (extended-VA path only).
//
// Allocates a contiguous < 4 GB host-RAM region for the chip to write its
// OUTFEED data into.  After inference completes, caller is responsible for
// memcpy(bounce -> user buffer) before freeing.
//
// ApexAllocOutputBounce:
//   - Size is page-aligned up internally (must be > 0).
//   - Pfns (length >= ceil(Size/PAGE_SIZE)) is filled with the bounce
//     buffer's per-page PFN sequence.  Caller passes Pfns straight to
//     ApexExtMapBuffer.
//   - Bounce kVA / PA / size are saved into DEVICE_CONTEXT for later
//     memcpy + free.
//   - Pre-fills bounce with sentinel byte 0xCC so we can tell post-INFER
//     whether OUTFEED actually wrote.
//
// ApexFreeOutputBounce:
//   - Idempotent.  Returns immediately if no bounce is active.
//   - Caller must already have done the bounce -> user memcpy (we don't
//     touch user memory here).
// =========================================================================
NTSTATUS ApexAllocOutputBounce(
    _In_  WDFDEVICE   Device,
    _In_  SIZE_T      Size,
    _Out_writes_(NumPagesOut) PPFN_NUMBER Pfns,
    _In_  UINT32      MaxPages,
    _Out_ UINT32     *NumPagesOut
);

VOID ApexFreeOutputBounce(_In_ WDFDEVICE Device);
