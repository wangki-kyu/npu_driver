#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <initguid.h>


EXTERN_C_START

typedef struct _DEVICE_CONTEXT
{
	// BAR2
	PVOID Bar2BaseAddress;	// MmMapIoSpace
	ULONG Bar2Length;		//

	// Interrupt
	WDFINTERRUPT Interrupts[1];

	// Page table for model/data memory mapping
	PVOID PageTableBase;                 // Page table kernel VA
	PHYSICAL_ADDRESS PageTablePhys;      // Page table physical address
	ULONG PageTableSize;                 // Number of allocated page table entries

	// Synchronization
	WDFSPINLOCK PageTableLock;           // Protect page table access

	// Device status
	ULONG DeviceStatus;                  // DEVICE_STATUS_DEAD/ALIVE

	// I/O Queue
	WDFQUEUE IoQueue;                    // Default I/O queue for IOCTLs

	// Locked memory tracking (keep pages locked during inference)
	PMDL LockedModelMdl;                 // MDL for locked model pages
	UINT64 LockedModelSize;              // Size of locked model

	ULONG PrivateDeviceData;  // just a placeholder
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS
npudriverCreateDevice(
	_Inout_ PWDFDEVICE_INIT DeviceInit
);

EVT_WDF_DEVICE_PREPARE_HARDWARE npudriverEvtDevicePrepareHardware;

EVT_WDF_DEVICE_RELEASE_HARDWARE npudriverEvtDeviceReleaseHardware;

EVT_WDF_FILE_CLEANUP npudriverEvtFileCleanup;

EXTERN_C_END