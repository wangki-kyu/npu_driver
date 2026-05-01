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
	UINT64 LastMapBufferDeviceVA;        // device VA from most recent IOCTL_MAP_BUFFER (used to remember
	                                     // the extended VA when cached PARAM bitstream takes ownership)

	// Extra MDL slots — when IOCTL_MAP_BUFFER is called multiple times
	// before any IOCTL_UNMAP_BUFFER / IOCTL_PARAM_CACHE handover, the
	// previous LockedModelMdl gets pushed here so it's not leaked.
	// Released at file cleanup.
	#define APEX_MAX_EXTRA_LOCKED_MDLS 16
	PMDL    ExtraLockedMdls[APEX_MAX_EXTRA_LOCKED_MDLS];
	UINT32  ExtraLockedMdlCount;

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
	UINT64  CachedParamDeviceVA;         // device VA the params were mapped at (extended or simple)
	UINT32  CachedParamPteIdx;           // First PTE index for params (simple-VA mode only)
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

	// Instruction queue descriptor ring (PTE slot 4096, deviceVA=0x1000000 — working trace)
	PVOID   DescRingBase;        // kernel VA (NonPagedPoolNx, 4KB)
	UINT64  DescRingDeviceVA;    // device virtual address seen by hardware
	UINT32  DescRingTail;        // monotonic submitted descriptor count

	// Status block (hardware DMA-writes completion info here, PTE slot 4097, deviceVA=0x1001000 — working trace)
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

	// SC_HOST_INT_COUNT (BAR2+0x486d0) cached at last completion-handoff point.
	// libedgetpu uses the *delta* against this register to detect new INFER
	// completions — the register is monotonic and increments each time SCALAR
	// executes its host_interrupt 0 opcode (placed AFTER OUTFEED drain barrier).
	// Reset at IOCTL_INFER entry (snapshot pre-submit value) so any post-submit
	// increment is unambiguously "this INFER completed".
	volatile UINT64 LastScHostIntCount;

	// Input image XOR-fold checksum computed at pre-submit time. Compared
	// against post-DONE recomputation to detect chip stray writes into
	// input region (chip should ONLY read from input, never write).
	// Equality alone doesn't prove the chip read the bytes, but combined
	// with INFEED kRun observation + PTE readback OK it's strong evidence
	// that the data the chip saw is exactly what we placed in host RAM.
	UINT64 InputChecksumPreSubmit;
	UINT64 InputChecksumByteCount;

	// =====================================================================
	// Extended-VA second-level page table pool.
	//
	// libedgetpu lays out every buffer (PARAM bitstream, INFER bitstream,
	// INPUT, OUTPUT[*], cached PARAM data) in the extended VA space starting
	// at 0x8000000000000000.  Each 2 MB chunk of extended VA needs its own
	// second-level page table page, indexed by chip PTE[6144 + (VA>>21)&0x1FFF].
	//
	// Multiple buffers can share one 2 MB region (different host_idx slots in
	// the same second-level PT).  The pool below tracks all currently-active
	// second-level PTs so we can:
	//   1) reuse the same PT when another buffer maps into the same 2 MB chunk
	//   2) free them at IOCTL_UNMAP_BUFFER / cleanup time
	//
	// Layout per slot:
	//   Kva        : kernel VA of 4 KB second-level PT page (MmNonCached, < 4 GB)
	//   Pa         : PA of that page (written into chip PTE register)
	//   ChipPteIdx : 6144 + (VA>>21)&0x1FFF (the chip register slot we wrote)
	//   Active     : TRUE while this slot is mapped, FALSE = free slot
	//
	// MAX_SUBTABLES is sized for typical workload: PARAM data may need 3 slots
	// (1500 pages = 6 MB across 3 chunks), plus 1 slot for INFER bitstream/INPUT/
	// OUTPUT/PARAM bitstream.  16 is comfortably above that.
	//
	// Legacy single-slot fields (ExtSecondLevelKva/Pa/ChipPteIdx) kept for
	// backward compatibility with diagnostics that reference the "primary"
	// extended mapping.  They mirror ExtSubtables[0] when active.
	#define APEX_MAX_EXT_SUBTABLES 16
	struct {
		PVOID   Kva;
		UINT64  Pa;
		UINT32  ChipPteIdx;
		BOOLEAN Active;
	} ExtSubtables[APEX_MAX_EXT_SUBTABLES];

	// Legacy fields — alias for ExtSubtables[0] (kept so diagnostic dumps work).
	PVOID    ExtSecondLevelKva;
	UINT64   ExtSecondLevelPa;
	UINT32   ExtChipPteIdx;
	BOOLEAN  ExtMappingActive;

	// Output bounce buffer (extended-VA path only).
	//
	// User-mode OUTPUT buffer pages live wherever the OS placed them — almost
	// always > 4 GB on 16+ GB systems.  We hit a chip silicon issue where
	// outbound write TLPs from OUTFEED to a >4 GB target PA caused HIB_ERR=0x1
	// + FATAL_ERR (chip MMU walk fails the inbound 2-level PT read OR truncates
	// the data PA to lower 32 bits).  Workaround: kernel allocates a contiguous
	// < 4 GB bounce buffer, maps THAT into the chip's 2-level PT, lets the chip
	// write into the bounce, then memcpy bounce → user MDL kernel VA in DPC
	// before unlocking.  Bounce is MmNonCached so chip writes are immediately
	// visible to the CPU at memcpy time.
	PVOID    OutputBounceKva;       // < 4 GB contiguous kernel VA; NULL = inactive
	UINT64   OutputBouncePa;        // base PA (saved for diagnostics)
	SIZE_T   OutputBounceSize;      // page-aligned size used for chip mapping
	BOOLEAN  OutputBounceActive;    // TRUE while bounce holds chip writes

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