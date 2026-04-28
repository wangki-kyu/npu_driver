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

	// BAR0 — diagnostic mapping. Windows may have placed MSI-X table here per
	// standard PCI MSI-X capability. We map and dump to find where MSI address/data
	// actually live so we can mirror them into chip's BAR2 MSI-X table at 0x46800.
	PVOID Bar0BaseAddress;
	ULONG Bar0Length;

	// Interrupt — chip allocates 4 MSI-X vectors. We register all 4 so that
	// whichever vector chip targets, our ISR receives it. The single ISR uses
	// MessageID to disambiguate.
	WDFINTERRUPT Interrupts[4];

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

	// Inference MDL tracking (DPC에서 unlock)
	PMDL InferInputMdl;                  // MDL for input image pages
	PMDL InferOutputMdl;                 // MDL for output buffer pages
	PMDL InferScratchMdl;                // MDL for scratch buffer pages

	// Cached parameters from IOCTL_PARAM_CACHE — kept mapped during INFER.
	// libedgetpu MapParameters() 패턴: 모든 executable 의 파라미터는 driver lifetime 동안
	// 매핑 유지. INFER bitstream 의 BASE_ADDRESS_PARAMETER 패치가 같은 VA 를 참조함.
	PMDL    CachedParamMdl;              // MDL for locked param pages (NULL = none)
	UINT32  CachedParamPteIdx;           // First PTE index for params
	UINT32  CachedParamPageCount;        // Number of param pages

	// Inference 완료 동기화 (IOCTL이 대기, DPC가 signal)
	KEVENT InferCompleteEvent;           // Event for inference completion

	// Instruction queue descriptor ring (PTE slot 6142, deviceVA=0x17FE000)
	PVOID   DescRingBase;        // kernel VA (NonPagedPoolNx, 4KB)
	UINT64  DescRingDeviceVA;    // device virtual address seen by hardware
	UINT32  DescRingTail;        // monotonic submitted descriptor count

	// Status block (hardware DMA-writes completion info here, PTE slot 6143)
	PVOID   StatusBlockBase;     // kernel VA (NonPagedPoolNx, 4KB)
	UINT64  StatusBlockDeviceVA; // device virtual address seen by hardware

	// ISR diagnostic counter (incremented on every ISR call, including spurious)
	volatile LONG IsrCallCount;

	// MSI-X table snapshot taken at PrepareHardware entry (before GCB reset wipes
	// chip's internal SRAM that backs BAR2+0x46800). Restored just before the
	// mask-bit clear so the chip has Windows-programmed addr/data again.
	// Mirrors Linux gasket_interrupt_reinit_msix() behavior.
	UINT32 SavedMsixTable[4 * 4];        // 4 vectors x {addr_lo, addr_hi, data, ctrl}
	BOOLEAN MsixTableSaved;

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

EVT_WDF_INTERRUPT_ISR npudriverEvtInterruptIsr;

EVT_WDF_INTERRUPT_DPC npudriverEvtInterruptDpc;

EVT_WDF_INTERRUPT_ENABLE  npudriverEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE npudriverEvtInterruptDisable;

EXTERN_C_END