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

	// Saved for DPC-time diagnostics (read OUTFEED status, dump output buffer
	// from kernel VA, verify PTE readback) BEFORE we unlock the output MDL.
	UINT64 InferOutputDeviceVA;          // device VA where OUTFEED writes results
	UINT64 InferOutputSize;              // output buffer size in bytes
	UINT64 InferInputDeviceVA;           // input device VA (for symmetric PTE check)
	UINT64 InferInputSize;               // input buffer size in bytes

	// Cached parameters from IOCTL_PARAM_CACHE — kept mapped during INFER.
	// libedgetpu MapParameters() 패턴: 모든 executable 의 파라미터는 driver lifetime 동안
	// 매핑 유지. INFER bitstream 의 BASE_ADDRESS_PARAMETER 패치가 같은 VA 를 참조함.
	PMDL    CachedParamMdl;              // MDL for locked param pages (NULL = none)
	UINT32  CachedParamPteIdx;           // First PTE index for params
	UINT32  CachedParamPageCount;        // Number of param pages

	// Cached PARAM_CACHE bitstream — kept mapped so IOCTL_INFER_WITH_PARAM can
	// re-enqueue it back-to-back with the INFER bitstream in the same IQ batch.
	// This is libedgetpu's pattern: PARAM_CACHE descriptor is submitted at every
	// inference to "activate" the cached params; engines have no idle gap between
	// the two bitstreams so they never auto-halt.
	PMDL    CachedParamBitstreamMdl;     // MDL for locked param-bitstream pages
	UINT64  CachedParamBitstreamDeviceVA;// device VA where the bitstream is mapped (typically 0x0)
	UINT32  CachedParamBitstreamSize;    // bitstream size in bytes (e.g. 0x2650)
	UINT32  CachedParamBitstreamPteIdx;  // First PTE index for the bitstream
	UINT32  CachedParamBitstreamPageCount;// Number of pages occupied by the bitstream

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

	// Last WIRE_INT_PENDING read by ISR — captured before W1C ack so DPC can see
	// which interrupt source fired even though the register was cleared at DIRQL.
	volatile UINT64 LastIsrWirePending;

	// OR-accumulated pending bits across all ISR fires for the current INFER.
	// IOCTL_INFER zeros this at submission; ISR ORs the pending word in.
	// The "INFER complete" signal is APEX_WIRE_BIT_SC_HOST_0 (bit 4 = 0x10),
	// fired when SCALAR executes the bitstream's host_interrupt 0 opcode at
	// end-of-program (compiler-promised AFTER OUTFEED drain barrier).
	// APEX_WIRE_BIT_IQ_INT (bit 0 = 0x1) alone means "descriptors fetched"
	// — fires too early, SCALAR may still be running.
	// APEX_WIRE_BIT_FATAL_ERR (bit 12 = 0x1000) is HIB fatal error.
	volatile LONG IsrSeenPendingBits;

	// libedgetpu-style INFER 완료 판정.
	// 0x486d0 (SC_HOST_INT_COUNT) 는 chip 이 SCALAR host_interrupt 0 opcode 를
	// 실행할 때마다 누적 증가. INFER bitstream 끝의 host_interrupt 0 = OUTFEED
	// drain barrier 뒤. 따라서 `count delta == expected` 가 진짜 INFER 완료.
	//   IOCTL_INFER          → expected = 1 (INFER bitstream 1 개)
	//   IOCTL_INFER_WITH_PARAM → expected = 2 (PARAM bitstream + INFER bitstream)
	// IQ_COMPLETED_HEAD / SC_HOST_0 비트만 보면 PARAM fetch/완료가 INFER 완료로
	// 오해됨 (OUTFEED 가 아직 시작도 안 했는데 unlock 하는 버그).
	UINT64 PreInferScHostIntCount;       // submit 직전에 저장한 count baseline
	UINT32 ExpectedScHostIncrement;      // 1 (INFER) / 2 (INFER_WITH_PARAM)
	volatile UINT64 LatestScHostIntCount;// ISR 가 매 SC_HOST IRQ 에서 read 해서 갱신

	// Input image XOR-fold checksum computed at pre-submit time. Compared
	// against post-DONE recomputation to detect chip stray writes into
	// input region (chip should ONLY read from input, never write).
	// Equality alone doesn't prove the chip read the bytes, but combined
	// with INFEED kRun observation + PTE readback OK it's strong evidence
	// that the data the chip saw is exactly what we placed in host RAM.
	UINT64 InputChecksumPreSubmit;
	UINT64 InputChecksumByteCount;

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