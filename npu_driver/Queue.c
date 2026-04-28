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

		DbgPrint("[%s] MAP_BUFFER: UserAddr=0x%llx Size=0x%llx ReqDeviceVA=0x%llx\n",
			__FUNCTION__, pInput->UserAddress, pInput->Size, pInput->DeviceAddress);
		{
			PDEVICE_CONTEXT pDC = DeviceGetContext(device);
			DbgPrint("[MAP] HIB_ERROR before map = 0x%llx\n",
				apex_read_register(pDC->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS));
		}

		// Caller-specified device VA — driver writes PTE[DeviceAddress>>12 ..]
		deviceAddr = pInput->DeviceAddress;
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
	case IOCTL_INFER_WITH_PARAM:
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
		// IOCTL_INFER_WITH_PARAM: enqueue cached PARAM bitstream descriptor BEFORE
		// the INFER descriptor in the same IQ batch. libedgetpu pattern — chip's
		// engines never see an idle gap between PARAM and INFER, so they don't
		// auto-halt and don't need wake-up.
		BOOLEAN withParam = (IoControlCode == IOCTL_INFER_WITH_PARAM);
		if (withParam) {
			if (pDevContext->CachedParamBitstreamMdl == NULL ||
			    pDevContext->CachedParamBitstreamSize == 0) {
				DbgPrint("[INFER_WITH_PARAM] No cached PARAM bitstream — call IOCTL_PARAM_CACHE first\n");
				status = STATUS_INVALID_DEVICE_STATE;
				break;
			}
		}

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
			DbgPrint("[%s] ISR call count BEFORE INFER: %d\n",
				__FUNCTION__, pDevContext->IsrCallCount);
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

		// 3. Allocate and lock scratch buffer (if provided)
		PMDL scratchMdl = NULL;
		if (pInput->ScratchAddr != 0 && pInput->ScratchSize > 0) {
			scratchMdl = IoAllocateMdl((PVOID)pInput->ScratchAddr,
									   (ULONG)pInput->ScratchSize, FALSE, FALSE, NULL);
			if (scratchMdl == NULL) {
				DbgPrint("[%s] IoAllocateMdl failed for scratch buffer\n", __FUNCTION__);
				MmUnlockPages(inputImageMdl);
				IoFreeMdl(inputImageMdl);
				MmUnlockPages(outputBufferMdl);
				IoFreeMdl(outputBufferMdl);
				status = STATUS_INSUFFICIENT_RESOURCES;
				break;
			}

			__try {
				MmProbeAndLockPages(scratchMdl, UserMode, IoWriteAccess);
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				DbgPrint("[%s] Failed to lock scratch buffer pages\n", __FUNCTION__);
				IoFreeMdl(scratchMdl);
				scratchMdl = NULL;
				MmUnlockPages(inputImageMdl);
				IoFreeMdl(inputImageMdl);
				MmUnlockPages(outputBufferMdl);
				IoFreeMdl(outputBufferMdl);
				status = STATUS_INVALID_PARAMETER;
				break;
			}
		}

		// 4. Save MDLs for cleanup
		pDevContext->InferInputMdl   = inputImageMdl;
		pDevContext->InferOutputMdl  = outputBufferMdl;
		pDevContext->InferScratchMdl = scratchMdl;
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

			// == PTE write read back == 
			{
				PUCHAR kvAddr = (PUCHAR)MmGetSystemAddressForMdlSafe(inputImageMdl, NormalPagePriority);
				UINT32 verifyCount = min(pageCount, 4);

				for (i = 0; i < verifyCount; i++) {
					UINT64 expectedPA = (UINT64)pfnArray[i] << PAGE_SHIFT;
					UINT64 readbackPA = apex_read_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8));
					UINT64 readbackPA_noFlag = readbackPA & ~1ULL;

					UINT64 vaFirstQword = (kvAddr != NULL) ? *(UINT64*)(kvAddr + i * PAGE_SIZE) : 0xDEADDEAD;

					DbgPrint("[%s] PTE[%lu+%u]: expected=0x%llX readback=0x%llX %s | VA[0]=0x%llX\n",
						__FUNCTION__, pteIdx, i,
						expectedPA, readbackPA_noFlag,
						(readbackPA_noFlag == expectedPA) ? "OK" : "MISMATCH",
						vaFirstQword);
				}
			}
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

		// 5b. Register scratch buffer pages in page table
		if (pDevContext->InferScratchMdl != NULL) {
			pteIdx = (UINT32)(pInput->ScratchDeviceVA >> PAGE_SHIFT);
			pageCount = (UINT32)((pInput->ScratchSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
			pfnArray = MmGetMdlPfnArray(pDevContext->InferScratchMdl);

			DbgPrint("[%s] Registering scratch: PTE[%lu] size=%lu pages\n",
				__FUNCTION__, pteIdx, pageCount);

			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pageCount; i++) {
				UINT64 physAddr = ((UINT64)pfnArray[i] << PAGE_SHIFT);
				apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), physAddr | 1);
			}
			WdfSpinLockRelease(pDevContext->PageTableLock);
		}

		// Force ALL engines to kRun (RUN_CONTROL=1) — including tile/mesh/ring/
		// outfeed/avdata which previous logs showed at status=0 (kIdle) before
		// INFER. INFER bitstream assumes every engine is kRun; PARAM bitstream
		// only wakes a subset (SCALAR/INFEED/PARAM_POP). libedgetpu does this
		// once at DoOpen via run_controller.DoRunControl(kMoveToRun) but we've
		// observed our chip silently ignoring those writes during PrepareHardware,
		// so we re-issue here with a longer settle delay.
		{
			UINT32 scBefore     = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT32 infeedBefore = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
			UINT32 paramBefore  = apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS);
			UINT32 outBefore    = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
			UINT32 avBefore     = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
			UINT32 tileBefore   = apex_read_register_32(bar2, APEX_REG_TILE_OP_RUN_STATUS);
			UINT32 m0Before     = (UINT32)apex_read_register(bar2, APEX_REG_MESH_BUS0_RUN_STATUS);
			UINT32 rcBefore     = (UINT32)apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS);
			UINT32 rpBefore     = (UINT32)apex_read_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_STATUS);
			DbgPrint("[INFER] pre-wake: SC=0x%x IN=0x%x PA=0x%x OUT=0x%x AV=0x%x TILE=0x%x MESH=0x%x RC=0x%x RP=0x%x\n",
				scBefore, infeedBefore, paramBefore, outBefore, avBefore,
				tileBefore, m0Before, rcBefore, rpBefore);

			// Confirm TILE_CONFIG0 first — engines may refuse RUN_CONTROL when tile
			// config is wrong. Re-write to be safe.
			apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
			KeStallExecutionProcessor(50);

			// kMoveToRun(1) to EVERY engine, regardless of current status.
			// Don't skip SCALAR — if it's already kRun the write is a no-op.
			apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL,             1);
			apex_write_register_32(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL,         1);
			apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL,      1);
			apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL,             1);
			apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL,            1);
			apex_write_register_32(bar2, APEX_REG_TILE_OP_RUN_CONTROL,            1);
			apex_write_register_32(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL,     1);
			apex_write_register_32(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL,     1);
			apex_write_register_32(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL,          1);
			apex_write_register_32(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL,          1);
			apex_write_register_32(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL,          1);
			apex_write_register_32(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL,          1);
			apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);
			apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);
			apex_write_register_32(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL,  1);

			// Settle. 1 ms — much longer than previous 200us.
			KeStallExecutionProcessor(1000);

			UINT32 scAfter     = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT32 infeedAfter = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
			UINT32 paramAfter  = apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS);
			UINT32 outAfter    = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
			UINT32 avAfter     = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
			UINT32 tileAfter   = apex_read_register_32(bar2, APEX_REG_TILE_OP_RUN_STATUS);
			UINT32 m0After     = (UINT32)apex_read_register(bar2, APEX_REG_MESH_BUS0_RUN_STATUS);
			UINT32 rcAfter     = (UINT32)apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS);
			UINT32 rpAfter     = (UINT32)apex_read_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_STATUS);
			DbgPrint("[INFER] post-wake: SC=0x%x IN=0x%x PA=0x%x OUT=0x%x AV=0x%x TILE=0x%x MESH=0x%x RC=0x%x RP=0x%x\n",
				scAfter, infeedAfter, paramAfter, outAfter, avAfter,
				tileAfter, m0After, rcAfter, rpAfter);
		}

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

		// 
		//UINT64 pteValue = apex_read_register(bar2, APEX_REG_PAGE_TABLE + (0 * 8));

		// Pre-submit engine state — 데이터/타일 엔진이 실제 kRunning 인지 확인
		DbgPrint("[INFER] pre-submit SCALAR=0x%x AVDATA=0x%x OUTFEED=0x%x INFEED=0x%x PARAM=0x%x IQ_INT=0x%llx WIRE=0x%llx\n",
			apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS),
			apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS),
			apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS),
			apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS),
			apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS),
			apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING));
		DbgPrint("[INFER] pre-submit tile: TILEOP=0x%x N2W=0x%llx W2N=0x%llx MESH0=0x%llx RINGP=0x%llx RINGC0=0x%llx TILE_CONFIG0=0x%llx\n",
			apex_read_register_32(bar2, APEX_REG_TILE_OP_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_MESH_BUS0_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS),
			apex_read_register(bar2, APEX_REG_TILE_CONFIG0));

		// 6. Submit HostQueueDescriptor(s) to ring.
		//    IOCTL_INFER:            single INFER descriptor.
		//    IOCTL_INFER_WITH_PARAM: PARAM_CACHE descriptor + INFER descriptor
		//                            back-to-back in the same TAIL update.
		{
			typedef struct {
				UINT64 address;
				UINT32 size_in_bytes;
				UINT32 reserved;
			} HOST_QUEUE_DESC;

			HOST_QUEUE_DESC *ring = (HOST_QUEUE_DESC *)pDevContext->DescRingBase;

			if (withParam) {
				UINT32 slotP = pDevContext->DescRingTail % 256;
				ring[slotP].address       = pDevContext->CachedParamBitstreamDeviceVA;
				ring[slotP].size_in_bytes = pDevContext->CachedParamBitstreamSize;
				ring[slotP].reserved      = 0;

				UINT32 slotI = (pDevContext->DescRingTail + 1) % 256;
				ring[slotI].address       = pInput->BitstreamDeviceVA;
				ring[slotI].size_in_bytes = (UINT32)pInput->BitstreamSize;
				ring[slotI].reserved      = 0;
				KeMemoryBarrier();

				pDevContext->DescRingTail += 2;
				apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDevContext->DescRingTail);

				UINT64 completed_head = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				DbgPrint("[INFER_WITH_PARAM] PARAM slot=%u VA=0x%llx size=0x%x | INFER slot=%u VA=0x%llx size=0x%x | TAIL=%u Completed=%llu\n",
					slotP, ring[slotP].address, ring[slotP].size_in_bytes,
					slotI, ring[slotI].address, ring[slotI].size_in_bytes,
					pDevContext->DescRingTail, completed_head);
			} else {
				UINT32 slot = pDevContext->DescRingTail % 256;
				ring[slot].address       = pInput->BitstreamDeviceVA;
				ring[slot].size_in_bytes = (UINT32)pInput->BitstreamSize;
				ring[slot].reserved      = 0;
				KeMemoryBarrier();

				pDevContext->DescRingTail++;
				apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDevContext->DescRingTail);

				UINT64 completed_head = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				DbgPrint("[%s] Descriptor submitted: slot=%u VA=0x%llx size=0x%x TAIL=%u Completed Head=%llu\n",
					__FUNCTION__, slot,
					ring[slot].address, ring[slot].size_in_bytes, pDevContext->DescRingTail, completed_head);

				UINT64 *rawRing = (UINT64 *)pDevContext->DescRingBase;
				DbgPrint("[INFER] Ring slot raw: [0]=0x%llx [1]=0x%llx\n",
					rawRing[slot * 2], rawRing[slot * 2 + 1]);
			}
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
			UINT64 qStatusBlockBase = apex_read_register(bar2, 0x48598);
			DbgPrint("[DIAG] STATUS_BLOCK_BASE=0x%llx\n", qStatusBlockBase);
		}

		// HIB 에러 디코딩 (descriptor 제출 직후)
		{
			UINT64 hibErr = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[INFER] POST-SUBMIT HIB_ERROR(64) = 0x%llx\n", hibErr);
			if (hibErr & (1ULL<<5)) DbgPrint("[INFER]   bit5: instruction_queue_bad_configuration\n");
			if (hibErr & (1ULL<<7)) DbgPrint("[INFER]   bit7: param_queue_bad_configuration\n");
			if (hibErr & (1ULL<<9)) DbgPrint("[INFER]   bit9: instruction_queue_invalid\n");
		}

		// Early poll: catch INFEED/OUTFEED/fault transitions in first 20ms
		{
			int qi;
			for (qi = 0; qi < 20; qi++) {
				LARGE_INTEGER d; d.QuadPart = -10000LL; // 1ms
				KeDelayExecutionThread(KernelMode, FALSE, &d);
				UINT32 inf      = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
				UINT32 out      = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
				UINT32 avd      = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
				UINT32 hibErr   = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
				UINT64 faultVA  = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
				UINT64 infFault = apex_read_register(bar2, APEX_REG_INFEED_PAGE_FAULT_ADDR);
				DbgPrint("[POLL@%dms] INFEED=0x%x OUTFEED=0x%x AVDATA=0x%x | HIB_ERR=0x%x FAULT_VA=0x%llx INFEED_FAULT=0x%llx\n",
					qi + 1, inf, out, avd, hibErr, faultVA, infFault);
				if (hibErr != 0 || out != 0) break;
			}
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
			// Stage 1 expired — check IQ completion first.
			// COMPLETED_HEAD is a ring index that wraps at queue_size; compare wrapped TAIL.
			UINT32 iqSize        = (UINT32)apex_read_register(bar2, APEX_REG_INSTR_QUEUE_SIZE);
			UINT32 expectedDone  = pDevContext->DescRingTail & (iqSize - 1);
			UINT32 scStatus50ms  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT64 completed50ms = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 hibErr50ms    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[50MS] SCALAR_RUN_STATUS=0x%x COMPLETED=0x%llx expected=%u TAIL=%u HIB_ERROR=0x%llx\n",
				scStatus50ms, completed50ms, expectedDone, pDevContext->DescRingTail, hibErr50ms);

			// Fast path: IQ already completed during stage 1 wait
			if ((UINT32)completed50ms == expectedDone) {
				DbgPrint("[50MS] IQ completed (COMPLETED=%llu == expected=%u TAIL=%u) — inference done\n",
					completed50ms, expectedDone, pDevContext->DescRingTail);
				status = STATUS_SUCCESS;
			}

			// Stage 2: poll COMPLETED_HEAD until == expectedDone (max 5s)
			if (status == STATUS_TIMEOUT) {
				int pollIdx;
				for (pollIdx = 0; pollIdx < 1000; pollIdx++) {
					UINT64 done = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
					if ((UINT32)done == expectedDone) {
						DbgPrint("[POLL] IQ completed after %d ms (COMPLETED=%llu == expected=%u TAIL=%u)\n",
							50 + pollIdx, done, expectedDone, pDevContext->DescRingTail);
						status = STATUS_SUCCESS;
						break;
					}
					// Periodic snapshot every 500ms
					if (pollIdx > 0 && pollIdx % 200 == 0) {
						UINT32 scSnap     = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
						UINT32 infeedSnap = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
						UINT32 outfedSnap = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
						UINT64 wirePend   = apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING);
						UINT64 hibSnap    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
						DbgPrint("[SNAP@%dms] SC=0x%x INFEED=0x%x OUTFEED=0x%x WIRE=0x%llx IQ_DONE=%llu HIB=0x%llx\n",
							50 + pollIdx, scSnap, infeedSnap, outfedSnap, wirePend, done, hibSnap);
					}
					{
						LARGE_INTEGER delay;
						delay.QuadPart = -10000LL; // 1ms
						KeDelayExecutionThread(KernelMode, FALSE, &delay);
					}
				}
			}

			if (status == STATUS_SUCCESS) {
				// Completed via polling — log post-completion diagnostics
				UINT32 outfeedSt = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
				UINT32 infeedSt  = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
				UINT64 hibErr    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
				UINT64 firstErr  = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
				DbgPrint("[DONE] OUTFEED=0x%x INFEED=0x%x HIB_ERROR=0x%llx HIB_FIRST=0x%llx\n",
					outfeedSt, infeedSt, hibErr, firstErr);
				DbgPrint("[DONE] ISR call count AFTER INFER: %d\n", pDevContext->IsrCallCount);
				if (hibErr & 1ULL)
					DbgPrint("[DONE]   inbound_page_fault at device VA 0x%llx\n", firstErr);

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
				if (pDevContext->InferScratchMdl) {
					MmUnlockPages(pDevContext->InferScratchMdl);
					IoFreeMdl(pDevContext->InferScratchMdl);
					pDevContext->InferScratchMdl = NULL;
				}
			}
			// if still not done, fall through to timeout handling below
		}

		if (status == STATUS_TIMEOUT) {
			DbgPrint("[%s] TIMEOUT waiting for inference\n", __FUNCTION__);
			DbgPrint("[TIMEOUT] ISR call count: %d (0 means MSI-X never delivered)\n",
				pDevContext->IsrCallCount);

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
			UINT32 paramStatus  = apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS);
			UINT64 deepSleep    = apex_read_register(bar2, APEX_REG_TILE_DEEP_SLEEP);
			// Switch to single-tile mode before reading 0x42xxx tile debug registers —
			// with TILE_CONFIG0=0x7F (broadcast) these reads return undefined results.
			apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x01);
			UINT32 tileOpStatus = apex_read_register_32(bar2, APEX_REG_TILE_OP_RUN_STATUS);
			UINT64 n2wStatus    = apex_read_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_STATUS);
			UINT64 w2nStatus    = apex_read_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_STATUS);
			UINT64 ringCons0    = apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS);
			UINT64 ringCons1    = apex_read_register(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_STATUS);
			UINT64 ringProd     = apex_read_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_STATUS);
			UINT64 mesh0        = apex_read_register(bar2, APEX_REG_MESH_BUS0_RUN_STATUS);
			apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
			UINT64 hibFirstErr       = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
			UINT64 infeedFaultAddr   = apex_read_register(bar2, APEX_REG_INFEED_PAGE_FAULT_ADDR);
			UINT32 axiQuiesce        = apex_read_register_32(bar2, APEX_REG_AXI_QUIESCE);
			UINT64 dmaPause          = apex_read_register(bar2, APEX_REG_USER_HIB_DMA_PAUSE);
			UINT64 dmaWasPaused      = apex_read_register(bar2, APEX_REG_USER_HIB_DMA_PAUSED);
			DbgPrint("[TIMEOUT] INFEED_PAGE_FAULT  = 0x%llx  (non-zero = INFEED MMU fault VA)\n", infeedFaultAddr);
			DbgPrint("[TIMEOUT] AXI_QUIESCE        = 0x%x    (must be 0 or scalar->DMA writes blocked)\n", axiQuiesce);
			DbgPrint("[TIMEOUT] DMA_PAUSE/PAUSED   = 0x%llx / 0x%llx\n", dmaPause, dmaWasPaused);
			DbgPrint("[TIMEOUT] WIRE_INT_PENDING   = 0x%llx\n", wirePending);
			DbgPrint("[TIMEOUT] SCALAR_RUN_STATUS  = 0x%llx\n", scalarStatus);
			DbgPrint("[TIMEOUT] INFEED_RUN_STATUS  = 0x%llx\n", infeedStatus);
			DbgPrint("[TIMEOUT] OUTFEED_RUN_STATUS = 0x%llx\n", outfeedStatus);
			DbgPrint("[TIMEOUT] AVDATA_RUN_STATUS  = 0x%x\n",   avdataStatus);
			DbgPrint("[TIMEOUT] PARAM_RUN_STATUS   = 0x%x\n",   paramStatus);
			DbgPrint("[TIMEOUT] TILE_DEEP_SLEEP    = 0x%llx\n", deepSleep);
			DbgPrint("[TIMEOUT] TILEOP_RUN_STATUS  = 0x%x\n",   tileOpStatus);
			DbgPrint("[TIMEOUT] N2W_RUN_STATUS     = 0x%llx\n", n2wStatus);
			DbgPrint("[TIMEOUT] W2N_RUN_STATUS     = 0x%llx\n", w2nStatus);
			DbgPrint("[TIMEOUT] RINGBUS_CONS0      = 0x%llx\n", ringCons0);
			DbgPrint("[TIMEOUT] RINGBUS_CONS1      = 0x%llx\n", ringCons1);
			DbgPrint("[TIMEOUT] RINGBUS_PROD       = 0x%llx\n", ringProd);
			DbgPrint("[TIMEOUT] MESHBUS0_STATUS    = 0x%llx\n", mesh0);
			DbgPrint("[TIMEOUT] IDLEGENERATOR      = 0x%08x\n", idleGen);
			DbgPrint("[TIMEOUT] USER_HIB_ERROR     = 0x%08x\n", hibError);
			DbgPrint("[TIMEOUT] HIB_FIRST_ERROR    = 0x%llx\n", hibFirstErr);
			if (hibError & 1) DbgPrint("[TIMEOUT]   inbound_page_fault at device VA 0x%llx\n", hibFirstErr);
			DbgPrint("[TIMEOUT] SCALAR_CORE_ERROR  = 0x%08x\n", scError);
			DbgPrint("[TIMEOUT] SC_HOST_INTVECCTL  = 0x%llx\n", scHostIntvec);
			DbgPrint("[TIMEOUT] WIRE_INT_MASK      = 0x%llx\n", wireIntMask);
			DbgPrint("[TIMEOUT] IQ_COMPLETED_HEAD  = 0x%llx\n", qCompleted);
			DbgPrint("[TIMEOUT] IQ_INT_STATUS      = 0x%llx\n", qIntStatus);
			DbgPrint("[TIMEOUT] IQ_CONTROL         = 0x%llx\n", qCtrl);

			// IQ 가 INFER 처리를 시작이라도 했는지: status block + ring slot 0 메모리 덤프
			if (pDevContext->StatusBlockBase != NULL) {
				UINT64 *sb = (UINT64 *)pDevContext->StatusBlockBase;
				DbgPrint("[TIMEOUT] StatusBlock: [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx\n",
					sb[0], sb[1], sb[2], sb[3]);
			}
			if (pDevContext->DescRingBase != NULL) {
				UINT64 *rr = (UINT64 *)pDevContext->DescRingBase;
				DbgPrint("[TIMEOUT] Ring slot0: [0]=0x%llx [1]=0x%llx | slot1: [0]=0x%llx [1]=0x%llx\n",
					rr[0], rr[1], rr[2], rr[3]);
			}
			DbgPrint("[TIMEOUT] IQ config: BASE=0x%llx SIZE=0x%llx DESC_SIZE=0x%llx STATUS_BLOCK=0x%llx FETCHED=0x%llx\n",
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_BASE),
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_SIZE),
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_DESC_SIZE),
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_STATUS_BLOCK),
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_FETCHED_HEAD));

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
			if (pDevContext->InferScratchMdl) {
				MmUnlockPages(pDevContext->InferScratchMdl);
				IoFreeMdl(pDevContext->InferScratchMdl);
				pDevContext->InferScratchMdl = NULL;
			}
			status = STATUS_IO_TIMEOUT;
		} else if (NT_SUCCESS(status)) {
			DbgPrint("[%s] Inference completed successfully\n", __FUNCTION__);
		}

		break;
	}

	case IOCTL_PARAM_CACHE:
	{
		WDFMEMORY inputMemory;
		IOCTL_PARAM_CACHE_INFO *pInput = NULL;
		PMDL paramMdl = NULL;
		PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);
		PVOID bar2 = pDevContext->Bar2BaseAddress;
		PPFN_NUMBER pfnArray;
		UINT32 pageCount, pteIdx, i;

		if (InputBufferLength < sizeof(IOCTL_PARAM_CACHE_INFO)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
		if (!NT_SUCCESS(status)) {
			DbgPrint("[%s] PARAM_CACHE: WdfRequestRetrieveInputMemory failed: 0x%x\n", __FUNCTION__, status);
			break;
		}

		pInput = (IOCTL_PARAM_CACHE_INFO *)WdfMemoryGetBuffer(inputMemory, NULL);
		DbgPrint("[PARAM_CACHE] ParamAddr=0x%llx Size=0x%llx DeviceVA=0x%llx BitstreamSize=0x%llx\n",
			pInput->ParamAddr, pInput->ParamSize, pInput->ParamDeviceVA, pInput->BitstreamSize);

		// 0. 이전에 caching 된 파라미터가 있으면 먼저 해제 (re-cache 케이스)
		if (pDevContext->CachedParamMdl != NULL) {
			DbgPrint("[PARAM_CACHE] Releasing previously cached params: PTE[%u..%u]\n",
				pDevContext->CachedParamPteIdx,
				pDevContext->CachedParamPteIdx + pDevContext->CachedParamPageCount - 1);
			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pDevContext->CachedParamPageCount; i++) {
				apex_write_register(bar2,
					APEX_REG_PAGE_TABLE + ((pDevContext->CachedParamPteIdx + i) * 8), 0);
			}
			WdfSpinLockRelease(pDevContext->PageTableLock);
			MmUnlockPages(pDevContext->CachedParamMdl);
			IoFreeMdl(pDevContext->CachedParamMdl);
			pDevContext->CachedParamMdl = NULL;
			pDevContext->CachedParamPteIdx = 0;
			pDevContext->CachedParamPageCount = 0;
		}

		// 0b. Release previously cached PARAM bitstream too (re-cache case)
		if (pDevContext->CachedParamBitstreamMdl != NULL) {
			DbgPrint("[PARAM_CACHE] Releasing previously cached bitstream: PTE[%u..%u]\n",
				pDevContext->CachedParamBitstreamPteIdx,
				pDevContext->CachedParamBitstreamPteIdx + pDevContext->CachedParamBitstreamPageCount - 1);
			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pDevContext->CachedParamBitstreamPageCount; i++) {
				apex_write_register(bar2,
					APEX_REG_PAGE_TABLE + ((pDevContext->CachedParamBitstreamPteIdx + i) * 8), 0);
			}
			WdfSpinLockRelease(pDevContext->PageTableLock);
			MmUnlockPages(pDevContext->CachedParamBitstreamMdl);
			IoFreeMdl(pDevContext->CachedParamBitstreamMdl);
			pDevContext->CachedParamBitstreamMdl = NULL;
			pDevContext->CachedParamBitstreamSize = 0;
			pDevContext->CachedParamBitstreamPteIdx = 0;
			pDevContext->CachedParamBitstreamPageCount = 0;
		}

		// 1. Lock parameter pages (hardware DMA reads them for PARAMETER_POP)
		paramMdl = IoAllocateMdl((PVOID)pInput->ParamAddr, (ULONG)pInput->ParamSize, FALSE, FALSE, NULL);
		if (paramMdl == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		__try {
			MmProbeAndLockPages(paramMdl, UserMode, IoReadAccess);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			DbgPrint("[PARAM_CACHE] Failed to lock parameter pages\n");
			IoFreeMdl(paramMdl);
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// 2. Register parameter pages in page table at ParamDeviceVA
		pteIdx    = (UINT32)(pInput->ParamDeviceVA >> PAGE_SHIFT);
		pageCount = (UINT32)((pInput->ParamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
		pfnArray  = MmGetMdlPfnArray(paramMdl);

		DbgPrint("[PARAM_CACHE] Registering param PTEs: PTE[%u..%u] (%u pages)\n",
			pteIdx, pteIdx + pageCount - 1, pageCount);
		WdfSpinLockAcquire(pDevContext->PageTableLock);
		for (i = 0; i < pageCount; i++)
			apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8),
				((UINT64)pfnArray[i] << PAGE_SHIFT) | 1);
		WdfSpinLockRelease(pDevContext->PageTableLock);

		// === MAPPING-ONLY MODE (always) ===
		//
		// libedgetpu pattern: MapParameters() only stages params + bitstream into
		// the chip's address space — it does NOT submit the PARAM bitstream to
		// the IQ. Submission happens only in RunInference(), where PARAM and
		// INFER descriptors are enqueued back-to-back.
		//
		// We follow the same pattern: IOCTL_PARAM_CACHE only locks pages and
		// programs PTEs. IOCTL_INFER_WITH_PARAM submits both descriptors. This
		// keeps engines in their PrepareHardware-set kRun/kIdle state until the
		// first inference runs, so kHalted=4 doesn't get latched between PARAM
		// and INFER.
		DbgPrint("[PARAM_CACHE] MAPPING-ONLY: PTEs registered, bitstream locked, NO chip submit\n");
		status = STATUS_SUCCESS;

		// 5. INFER 가 같은 PARAM_VA 를 참조하므로 PTE 와 lock 을 유지한다.
		//    libedgetpu MapParameters() 패턴: parameters 는 driver lifetime 동안 매핑.
		//    cleanup 은 npudriverEvtFileCleanup 또는 다음 PARAM_CACHE 호출 시.
		//
		//    또한 PARAM bitstream MDL (LockedModelMdl) 의 ownership 도 driver 가
		//    이어받아 IOCTL_INFER_WITH_PARAM 에서 매번 IQ 에 다시 enqueue 할 수
		//    있도록 함. test_console 은 PARAM bitstream 을 IOCTL_UNMAP_BUFFER 하지
		//    않아야 하며 (driver 가 cleanup 까지 유지), driver 가 LockedModelMdl=NULL
		//    로 만들어 다음 IOCTL_UNMAP_BUFFER 가 잘못 unlock 하는 것도 방지.
		if (NT_SUCCESS(status)) {
			pDevContext->CachedParamMdl       = paramMdl;
			pDevContext->CachedParamPteIdx    = pteIdx;
			pDevContext->CachedParamPageCount = pageCount;
			DbgPrint("[PARAM_CACHE] Cached params retained: PTE[%u..%u] (%u pages, MDL=%p)\n",
				pteIdx, pteIdx + pageCount - 1, pageCount, paramMdl);

			// Transfer PARAM bitstream MDL ownership from LockedModelMdl to
			// CachedParamBitstreamMdl. Bitstream is at deviceVA=0 (PTE[0..N]).
			if (pDevContext->LockedModelMdl != NULL && pInput->BitstreamSize > 0) {
				UINT32 bsPageCount = (UINT32)((pInput->BitstreamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
				pDevContext->CachedParamBitstreamMdl       = pDevContext->LockedModelMdl;
				pDevContext->CachedParamBitstreamDeviceVA  = 0;
				pDevContext->CachedParamBitstreamSize      = (UINT32)pInput->BitstreamSize;
				pDevContext->CachedParamBitstreamPteIdx    = 0;
				pDevContext->CachedParamBitstreamPageCount = bsPageCount;
				pDevContext->LockedModelMdl  = NULL;
				pDevContext->LockedModelSize = 0;
				DbgPrint("[PARAM_CACHE] Cached bitstream retained: PTE[0..%u] DeviceVA=0x0 size=0x%x MDL=%p\n",
					bsPageCount - 1, (UINT32)pInput->BitstreamSize,
					pDevContext->CachedParamBitstreamMdl);
			}
		} else {
			// 실패 시에는 즉시 정리
			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pageCount; i++)
				apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), 0);
			WdfSpinLockRelease(pDevContext->PageTableLock);
			MmUnlockPages(paramMdl);
			IoFreeMdl(paramMdl);
		}
		DbgPrint("[PARAM_CACHE] Done, status=0x%x\n", status);
		break;
	}

	default:
		DbgPrint("[%s] Unknown IOCTL: 0x%x\n", __FUNCTION__, IoControlCode);
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
