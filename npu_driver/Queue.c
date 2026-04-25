#include "Driver.h"
#include "Queue.h"
#include "Memory.h"


#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, npudriverEvtIoDeviceControl)
#endif

VOID npudriverEvtIoDeviceControl(
	_In_ WDFQUEUE Queue,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
)
{
	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;

	UNREFERENCED_PARAMETER(OutputBufferLength);

	DbgPrint("[%s] IOCTL: 0x%x\n", __FUNCTION__, IoControlCode);

	switch (IoControlCode)
	{
	case IOCTL_MAP_BUFFER:
	{
		WDFMEMORY inputMemory;
		MAP_BUFFER_INPUT *pInput = NULL;
		UINT64 deviceAddr = 0;

		if (InputBufferLength < sizeof(MAP_BUFFER_INPUT)) {
			DbgPrint("[%s] Invalid input buffer size\n", __FUNCTION__);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
		if (!NT_SUCCESS(status)) {
			DbgPrint("[%s] WdfRequestRetrieveInputMemory failed: 0x%x\n", __FUNCTION__, status);
			break;
		}

		pInput = (MAP_BUFFER_INPUT *)WdfMemoryGetBuffer(inputMemory, NULL);

		DbgPrint("[%s] MAP_BUFFER: UserAddr=0x%llx, Size=0x%llx\n",
			__FUNCTION__, pInput->UserAddress, pInput->Size);
		{
			PDEVICE_CONTEXT pDC = DeviceGetContext(device);
			DbgPrint("[MAP] HIB_ERROR before map = 0x%llx\n",
				apex_read_register(pDC->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS));
		}

		status = ApexPageTableMap(device, (PVOID)pInput->UserAddress, (SIZE_T)pInput->Size, &deviceAddr);

		if (NT_SUCCESS(status)) {
			PDEVICE_CONTEXT pDC = DeviceGetContext(device);
			DbgPrint("[MAP] HIB_ERROR after map = 0x%llx\n",
				apex_read_register(pDC->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS));
			DbgPrint("[%s] Mapped successfully, DeviceAddr=0x%llx\n", __FUNCTION__, deviceAddr);
		}
		break;
	}

	case IOCTL_UNMAP_BUFFER:
	{
		WDFMEMORY inputMemory;
		UNMAP_BUFFER_INPUT *pInput = NULL;
		PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);

		if (InputBufferLength < sizeof(UNMAP_BUFFER_INPUT)) {
			DbgPrint("[%s] Invalid input buffer size\n", __FUNCTION__);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
		if (!NT_SUCCESS(status)) {
			DbgPrint("[%s] WdfRequestRetrieveInputMemory failed: 0x%x\n", __FUNCTION__, status);
			break;
		}

		pInput = (UNMAP_BUFFER_INPUT *)WdfMemoryGetBuffer(inputMemory, NULL);

		DbgPrint("[%s] UNMAP_BUFFER: DeviceAddr=0x%llx, Size=0x%llx\n",
			__FUNCTION__, pInput->DeviceAddress, pInput->Size);

		status = ApexPageTableUnmap(device, pInput->DeviceAddress, (SIZE_T)pInput->Size);

		if (NT_SUCCESS(status)) {
			DbgPrint("[%s] Unmapped successfully\n", __FUNCTION__);
		}

		// Unlock pages if they were locked
		DbgPrint("[%s] LockedModelMdl = %p, LockedModelSize = 0x%llx\n",
			__FUNCTION__, pDevContext->LockedModelMdl, pDevContext->LockedModelSize);

		if (pDevContext->LockedModelMdl != NULL) {
			DbgPrint("[%s] Unlocking %llu bytes...\n", __FUNCTION__, pDevContext->LockedModelSize);
			MmUnlockPages(pDevContext->LockedModelMdl);
			DbgPrint("[%s] MmUnlockPages done\n", __FUNCTION__);
			IoFreeMdl(pDevContext->LockedModelMdl);
			DbgPrint("[%s] IoFreeMdl done\n", __FUNCTION__);
			pDevContext->LockedModelMdl = NULL;
			pDevContext->LockedModelSize = 0;
			DbgPrint("[%s] Model pages unlocked successfully\n", __FUNCTION__);
		} else {
			DbgPrint("[%s] WARNING: LockedModelMdl is NULL, nothing to unlock\n", __FUNCTION__);
		}
		break;
	}
	case IOCTL_INFER:
	{
		WDFMEMORY inputMemory;
		IOCTL_INFER_INFO *pInput = NULL;
		PMDL inputImageMdl = NULL, outputBufferMdl = NULL;
		PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);
		PVOID bar2 = pDevContext->Bar2BaseAddress;
		PPFN_NUMBER pfnArray;
		UINT32 pageCount;
		UINT32 pteIdx;
		UINT32 i;

		if (InputBufferLength < sizeof(IOCTL_INFER_INFO)) {
			DbgPrint("[%s] Invalid input buffer size\n", __FUNCTION__);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
		if (!NT_SUCCESS(status)) {
			DbgPrint("[%s] WdfRequestRetrieveInputMemory failed: 0x%x\n", __FUNCTION__, status);
			break;
		}

		pInput = (IOCTL_INFER_INFO *)WdfMemoryGetBuffer(inputMemory, NULL);

		DbgPrint("[%s] IOCTL_INFER: Input image at 0x%llx (size 0x%llx)\n",
			__FUNCTION__, pInput->InputImageAddr, pInput->InputImageSize);
		DbgPrint("[%s] IOCTL_INFER: Output buffer at 0x%llx (size 0x%llx)\n",
			__FUNCTION__, pInput->OutputBufferAddr, pInput->OutputBufferSize);

		// Check error statuses before inference
		{
			UINT32 hibError = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			UINT32 scError = apex_read_register_32(bar2, APEX_REG_SCALAR_CORE_ERROR_STATUS);
			DbgPrint("[%s] USER_HIB_ERROR = 0x%08x, SCALAR_CORE_ERROR = 0x%08x\n",
				__FUNCTION__, hibError, scError);
		}

		// 1. Allocate and lock input image
		inputImageMdl = IoAllocateMdl((PVOID)pInput->InputImageAddr,
									   (ULONG)pInput->InputImageSize, FALSE, FALSE, NULL);
		if (inputImageMdl == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		__try {
			MmProbeAndLockPages(inputImageMdl, UserMode, IoReadAccess);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			DbgPrint("[%s] Failed to lock input image pages\n", __FUNCTION__);
			IoFreeMdl(inputImageMdl);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// 2. Allocate and lock output buffer
		outputBufferMdl = IoAllocateMdl((PVOID)pInput->OutputBufferAddr,
										(ULONG)pInput->OutputBufferSize, FALSE, FALSE, NULL);
		if (outputBufferMdl == NULL) {
			DbgPrint("[%s] IoAllocateMdl failed for output buffer\n", __FUNCTION__);
			MmUnlockPages(inputImageMdl);
			IoFreeMdl(inputImageMdl);
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		__try {
			MmProbeAndLockPages(outputBufferMdl, UserMode, IoWriteAccess);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			DbgPrint("[%s] Failed to lock output buffer pages\n", __FUNCTION__);
			MmUnlockPages(inputImageMdl);
			IoFreeMdl(inputImageMdl);
			IoFreeMdl(outputBufferMdl);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// 3. Save MDLs for DPC
		pDevContext->InferInputMdl = inputImageMdl;
		pDevContext->InferOutputMdl = outputBufferMdl;
		KeClearEvent(&pDevContext->InferCompleteEvent);

		// 4. Register input image pages in page table
		{
			pteIdx = (UINT32)(pInput->InputDeviceVA >> PAGE_SHIFT);
			pageCount = (UINT32)((pInput->InputImageSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
			pfnArray = MmGetMdlPfnArray(inputImageMdl);

			DbgPrint("[%s] Registering input: PTE[%lu] size=%lu pages\n",
				__FUNCTION__, pteIdx, pageCount);

			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pageCount; i++) {
				UINT64 physAddr = ((UINT64)pfnArray[i] << PAGE_SHIFT);
				apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), physAddr | 1);
			}
			WdfSpinLockRelease(pDevContext->PageTableLock);
		}

		// 5. Register output buffer pages in page table
		{
			pteIdx = (UINT32)(pInput->OutputDeviceVA >> PAGE_SHIFT);
			pageCount = (UINT32)((pInput->OutputBufferSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
			pfnArray = MmGetMdlPfnArray(outputBufferMdl);

			DbgPrint("[%s] Registering output: PTE[%lu] size=%lu pages\n",
				__FUNCTION__, pteIdx, pageCount);

			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pageCount; i++) {
				UINT64 physAddr = ((UINT64)pfnArray[i] << PAGE_SHIFT);
				apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), physAddr | 1);
			}
			WdfSpinLockRelease(pDevContext->PageTableLock);
		}

		// Configure tiles now that bitstream is mapped — tiles activate here and may
		// immediately access bitstream VAs, so PTE[0..N] must already be valid.
		apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
		apex_write_register(bar2, APEX_REG_TILE_DEEP_SLEEP, 0x1E02ULL);
		DbgPrint("[INFER] TILE_CONFIG0=0x7F TILE_DEEP_SLEEP=0x1E02\n");

		// Disable all breakpoints — after GCB reset the default is 0, which means
		// "halt at PC=0" and causes the scalar core to stop before executing one instruction.
		apex_write_register(bar2, APEX_REG_SCALAR_BREAKPOINT,        0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_INFEED_BREAKPOINT,        0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_OUTFEED_BREAKPOINT,       0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_PARAMETER_POP_BREAKPOINT, 0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_AVDATA_POP_BREAKPOINT,    0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_TILE_OP_BREAKPOINT,       0xFFFFFFFFFFFFFFFFULL);
		DbgPrint("[INFER] Breakpoints disabled (set to 0xFFFFFFFFFFFFFFFF)\n");

		// Set run controls now that bitstream is mapped (PTE[0..N] valid).
		// Must NOT be set in PrepareHardware — bitstream isn't mapped yet at that point.
		apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL,             1);
		apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL,             1);
		apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL,      1);
		apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL,            1);
		apex_write_register_32(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL,         1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL,  1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_TILE_OP_RUN_CONTROL,            1);
		apex_write_register_32(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL,     1);
		apex_write_register_32(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL,     1);
		DbgPrint("[INFER] Run controls set\n");
		
		// Verify PTE and queue config
		{
			UINT64 diagPte0     = apex_read_register(bar2, APEX_REG_PAGE_TABLE + 0);
			UINT64 diagQBase    = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_BASE);
			UINT64 diagRingIdx  = diagQBase >> 12;
			UINT64 diagPteRing  = apex_read_register(bar2, APEX_REG_PAGE_TABLE + diagRingIdx * 8);
			UINT64 diagDescSize = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_DESC_SIZE);
			UINT64 diagPtSize   = apex_read_register(bar2, APEX_REG_PAGE_TABLE_SIZE);
			DbgPrint("[DIAG] PTE[0]=0x%llx QUEUE_BASE=0x%llx PTE[%llu]=0x%llx\n",
				diagPte0, diagQBase, diagRingIdx, diagPteRing);
			DbgPrint("[DIAG] DESC_SIZE=%llu PAGE_TABLE_SIZE=%llu\n", diagDescSize, diagPtSize);
		}

		// 6. Submit HostQueueDescriptor to ring
		// libedgetpu model: BASE points to ring buffer, each entry = {address(8), size(4), reserved(4)}
		// TAIL = cumulative descriptor count (not byte offset)
		{
			typedef struct {
				UINT64 address;
				UINT32 size_in_bytes;
				UINT32 reserved;
			} HOST_QUEUE_DESC;

			HOST_QUEUE_DESC *ring = (HOST_QUEUE_DESC *)pDevContext->DescRingBase;
			UINT32 slot = pDevContext->DescRingTail % 256;
			ring[slot].address = pInput->BitstreamDeviceVA;
			ring[slot].size_in_bytes = (UINT32)pInput->BitstreamSize;
			ring[slot].reserved = 0;
			KeMemoryBarrier();

			pDevContext->DescRingTail++;
			apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDevContext->DescRingTail);

			DbgPrint("[%s] Descriptor submitted: slot=%u VA=0x%llx size=0x%x TAIL=%u\n",
				__FUNCTION__, slot,
				ring[slot].address, ring[slot].size_in_bytes, pDevContext->DescRingTail);
		}

		// Verify instruction queue state
		{
			UINT64 qBase      = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_BASE);
			UINT64 qSize      = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_SIZE);
			UINT64 qTail      = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_TAIL);
			UINT64 qFetch     = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_FETCHED_HEAD);
			UINT64 qComplete  = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 qCtrl      = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_CONTROL);
			UINT64 qIntStatus = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS);
			DbgPrint("[DIAG] QUEUE_BASE=0x%llx SIZE=0x%llx TAIL=0x%llx FETCHED=0x%llx COMPLETED=0x%llx CTRL=0x%llx INT_STATUS=0x%llx\n",
				qBase, qSize, qTail, qFetch, qComplete, qCtrl, qIntStatus);
		}

		// HIB 에러 디코딩 (descriptor 제출 직후)
		{
			UINT64 hibErr = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[INFER] POST-SUBMIT HIB_ERROR(64) = 0x%llx\n", hibErr);
			if (hibErr & (1ULL<<5)) DbgPrint("[INFER]   bit5: instruction_queue_bad_configuration\n");
			if (hibErr & (1ULL<<7)) DbgPrint("[INFER]   bit7: param_queue_bad_configuration\n");
			if (hibErr & (1ULL<<9)) DbgPrint("[INFER]   bit9: instruction_queue_invalid\n");
		}

		// 7. Wait for inference completion — 2-stage: 50ms then 950ms
		// Stage 1: Short wait (inference typically takes ~15ms)
		{
			LARGE_INTEGER shortTimeout;
			shortTimeout.QuadPart = -500000LL; // 50ms
			DbgPrint("[%s] Waiting for inference (stage 1: 50ms)...\n", __FUNCTION__);
			status = KeWaitForSingleObject(&pDevContext->InferCompleteEvent,
										   Executive, KernelMode, FALSE, &shortTimeout);
		}

		if (status == STATUS_TIMEOUT) {
			// Stage 1 expired — check if chip ran but interrupt was missed
			UINT32 scRunStatus50ms  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT64 wirePending50ms  = apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING);
			UINT32 idleGen50ms      = apex_read_register_32(bar2, APEX_REG_IDLEGENERATOR);
			UINT64 completed50ms    = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 hibErr50ms       = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[50MS] SCALAR_RUN_STATUS=0x%x WIRE_INT_PENDING=0x%llx IDLEGENERATOR=0x%x\n",
				scRunStatus50ms, wirePending50ms, idleGen50ms);
			DbgPrint("[50MS] COMPLETED_HEAD=0x%llx HIB_ERROR=0x%llx\n",
				completed50ms, hibErr50ms);

			if (scRunStatus50ms != 0) {
				// Chip completed inference but MSI-X interrupt was missed — succeed anyway
				DbgPrint("[50MS] Chip completed (STATUS=0x%x) but interrupt missed — signalling success\n",
					scRunStatus50ms);

				// Diagnose why output might be zero: check outfeed and page fault
				{
					UINT32 outfeedSt  = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
					UINT32 infeedSt   = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
					UINT64 hibErr     = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
					UINT64 firstErr   = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
					DbgPrint("[50MS] OUTFEED_STATUS=0x%x INFEED_STATUS=0x%x\n", outfeedSt, infeedSt);
					DbgPrint("[50MS] HIB_ERROR=0x%llx HIB_FIRST_ERROR(faulting VA)=0x%llx\n",
						hibErr, firstErr);
					if (hibErr & 1ULL)
						DbgPrint("[50MS]   inbound_page_fault at device VA 0x%llx\n", firstErr);
				}

				if (pDevContext->InferInputMdl) {
					MmUnlockPages(pDevContext->InferInputMdl);
					IoFreeMdl(pDevContext->InferInputMdl);
					pDevContext->InferInputMdl = NULL;
				}
				if (pDevContext->InferOutputMdl) {
					MmUnlockPages(pDevContext->InferOutputMdl);
					IoFreeMdl(pDevContext->InferOutputMdl);
					pDevContext->InferOutputMdl = NULL;
				}
				status = STATUS_SUCCESS;
			} else {
				// Stage 2: Chip hasn't finished yet, wait the remaining 950ms
				LARGE_INTEGER longTimeout;
				longTimeout.QuadPart = -9500000LL; // 950ms
				DbgPrint("[%s] Waiting for inference (stage 2: 950ms)...\n", __FUNCTION__);
				status = KeWaitForSingleObject(&pDevContext->InferCompleteEvent,
											   Executive, KernelMode, FALSE, &longTimeout);
			}
		}

		if (status == STATUS_TIMEOUT) {
			DbgPrint("[%s] TIMEOUT waiting for inference\n", __FUNCTION__);

			// Diagnostic: read hardware state to understand why
			UINT64 wirePending   = apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING);
			UINT64 scalarStatus  = apex_read_register(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT64 infeedStatus  = apex_read_register(bar2, APEX_REG_INFEED_RUN_STATUS);
			UINT64 outfeedStatus = apex_read_register(bar2, APEX_REG_OUTFEED_RUN_STATUS);
			UINT32 hibError      = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			UINT32 scError       = apex_read_register_32(bar2, APEX_REG_SCALAR_CORE_ERROR_STATUS);
			UINT64 scHostIntvec  = apex_read_register(bar2, APEX_REG_SC_HOST_INTVECCTL);
			UINT64 wireIntMask   = apex_read_register(bar2, APEX_REG_WIRE_INT_MASK);
			UINT32 idleGen       = apex_read_register_32(bar2, APEX_REG_IDLEGENERATOR);
			UINT64 qCompleted    = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 qIntStatus    = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS);
			UINT64 qCtrl         = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_CONTROL);

			UINT32 avdataStatus = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
			UINT32 tileOpStatus = apex_read_register_32(bar2, APEX_REG_TILE_OP_RUN_STATUS);
			UINT64 n2wStatus    = apex_read_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_STATUS);
			UINT64 w2nStatus    = apex_read_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_STATUS);
			UINT64 ringCons0    = apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS);
			UINT64 ringCons1    = apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_STATUS);
			UINT64 ringProd     = apex_read_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_STATUS);
			UINT64 mesh0        = apex_read_register(bar2, APEX_REG_MESH_BUS0_RUN_STATUS);
			DbgPrint("[TIMEOUT] WIRE_INT_PENDING   = 0x%llx\n", wirePending);
			DbgPrint("[TIMEOUT] SCALAR_RUN_STATUS  = 0x%llx\n", scalarStatus);
			DbgPrint("[TIMEOUT] INFEED_RUN_STATUS  = 0x%llx\n", infeedStatus);
			DbgPrint("[TIMEOUT] OUTFEED_RUN_STATUS = 0x%llx\n", outfeedStatus);
			DbgPrint("[TIMEOUT] AVDATA_RUN_STATUS  = 0x%x\n",   avdataStatus);
			DbgPrint("[TIMEOUT] TILEOP_RUN_STATUS  = 0x%x\n",   tileOpStatus);
			DbgPrint("[TIMEOUT] N2W_RUN_STATUS     = 0x%llx\n", n2wStatus);
			DbgPrint("[TIMEOUT] W2N_RUN_STATUS     = 0x%llx\n", w2nStatus);
			DbgPrint("[TIMEOUT] RINGBUS_CONS0      = 0x%llx\n", ringCons0);
			DbgPrint("[TIMEOUT] RINGBUS_CONS1      = 0x%llx\n", ringCons1);
			DbgPrint("[TIMEOUT] RINGBUS_PROD       = 0x%llx\n", ringProd);
			DbgPrint("[TIMEOUT] MESHBUS0_STATUS    = 0x%llx\n", mesh0);
			DbgPrint("[TIMEOUT] IDLEGENERATOR      = 0x%08x\n", idleGen);
			DbgPrint("[TIMEOUT] USER_HIB_ERROR     = 0x%08x\n", hibError);
			DbgPrint("[TIMEOUT] SCALAR_CORE_ERROR  = 0x%08x\n", scError);
			DbgPrint("[TIMEOUT] SC_HOST_INTVECCTL  = 0x%llx\n", scHostIntvec);
			DbgPrint("[TIMEOUT] WIRE_INT_MASK      = 0x%llx\n", wireIntMask);
			DbgPrint("[TIMEOUT] IQ_COMPLETED_HEAD  = 0x%llx\n", qCompleted);
			DbgPrint("[TIMEOUT] IQ_INT_STATUS      = 0x%llx\n", qIntStatus);
			DbgPrint("[TIMEOUT] IQ_CONTROL         = 0x%llx\n", qCtrl);

			// Cleanup
			if (pDevContext->InferInputMdl) {
				MmUnlockPages(pDevContext->InferInputMdl);
				IoFreeMdl(pDevContext->InferInputMdl);
				pDevContext->InferInputMdl = NULL;
			}
			if (pDevContext->InferOutputMdl) {
				MmUnlockPages(pDevContext->InferOutputMdl);
				IoFreeMdl(pDevContext->InferOutputMdl);
				pDevContext->InferOutputMdl = NULL;
			}
			status = STATUS_IO_TIMEOUT;
		} else if (NT_SUCCESS(status)) {
			DbgPrint("[%s] Inference completed successfully\n", __FUNCTION__);
		}

		break;
	}

	default:
		DbgPrint("[%s] Unknown IOCTL: 0x%x\n", __FUNCTION__, IoControlCode);
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
