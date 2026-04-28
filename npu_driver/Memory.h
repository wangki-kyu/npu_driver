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
