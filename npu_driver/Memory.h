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
// Extended-VA mapping helpers — coral.sys-style bulk pool.
//
// At ApexPageTableInit we allocate ONE 8 MB pool (2048 × 4 KB) covering all
// extended subtables, and pre-program every chip PTE register [6144..8191]
// to point at the corresponding 4 KB sub-region (valid bit set).  The pool
// is zeroed, so every extended slot is a valid (all-entries-zero) 2-level
// PT from the moment the chip starts walking.
//
// ApexExtMapBuffer:
//   - ExtDeviceVA must have bit 63 set, page-aligned
//   - Pfns is an array of host PFNs (PA >> 12).
//   - Writes per-page entries into the pre-allocated pool — no alloc.
//   - Crosses 2 MB region boundaries automatically (recurses).
//
// ApexExtUnmapBuffer:
//   - Kept as a no-op alias for backward compatibility.  Pool lives for the
//     device handle lifetime; per-buffer entry zeroing is handled by
//     ApexPageTableUnmap.
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
