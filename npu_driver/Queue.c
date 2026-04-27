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

		// Breakpoints, tile config, and run controls are now set once in
		// PrepareHardware (device open). All units are already kRunning here.
		
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
			// SCALAR_RUN_STATUS stays 0x1 forever (scalar idles in a poll loop between
			// inferences); COMPLETED_HEAD >= TAIL is the libedgetpu completion signal.
			UINT32 scStatus50ms  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT64 completed50ms = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 hibErr50ms    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[50MS] SCALAR_RUN_STATUS=0x%x COMPLETED=0x%llx TAIL=%u HIB_ERROR=0x%llx\n",
				scStatus50ms, completed50ms, pDevContext->DescRingTail, hibErr50ms);

			// Fast path: IQ already completed during stage 1 wait
			if (completed50ms >= (UINT64)pDevContext->DescRingTail) {
				DbgPrint("[50MS] IQ completed (COMPLETED=%llu >= TAIL=%u) — inference done\n",
					completed50ms, pDevContext->DescRingTail);
				status = STATUS_SUCCESS;
			}

			// Stage 2: poll COMPLETED_HEAD until >= TAIL (max 5s)
			if (status == STATUS_TIMEOUT) {
				int pollIdx;
				for (pollIdx = 0; pollIdx < 5000; pollIdx++) {
					UINT64 done = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
					if (done >= (UINT64)pDevContext->DescRingTail) {
						DbgPrint("[POLL] IQ completed after %d ms (COMPLETED=%llu TAIL=%u)\n",
							50 + pollIdx, done, pDevContext->DescRingTail);
						status = STATUS_SUCCESS;
						break;
					}
					// Periodic snapshot every 500ms
					if (pollIdx > 0 && pollIdx % 500 == 0) {
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

		// 3. Submit exe1 descriptor (bitstream at device VA 0x0, already mapped via IOCTL_MAP_BUFFER)
		{
			typedef struct { UINT64 address; UINT32 size_in_bytes; UINT32 reserved; } HOST_QUEUE_DESC;
			HOST_QUEUE_DESC *ring = (HOST_QUEUE_DESC *)pDevContext->DescRingBase;
			UINT32 slot = pDevContext->DescRingTail % 256;
			ring[slot].address       = 0;
			ring[slot].size_in_bytes = (UINT32)pInput->BitstreamSize;
			ring[slot].reserved      = 0;
			KeMemoryBarrier();
			pDevContext->DescRingTail++;
			apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDevContext->DescRingTail);
			DbgPrint("[PARAM_CACHE] Submitted exe1: slot=%u size=0x%x TAIL=%u\n",
				slot, (UINT32)pInput->BitstreamSize, pDevContext->DescRingTail);
		}

		// 4. Poll IQ completion (PARAMETER_POP loads ~6MB, allow up to 10s)
		{
			int pollIdx;
			status = STATUS_IO_TIMEOUT;
			for (pollIdx = 0; pollIdx < 10000; pollIdx++) {
				UINT64 done = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				if (done >= (UINT64)pDevContext->DescRingTail) {
					DbgPrint("[PARAM_CACHE] IQ completed after %d ms (COMPLETED=%llu TAIL=%u)\n",
						pollIdx, done, pDevContext->DescRingTail);
					status = STATUS_SUCCESS;
					break;
				}
				if (pollIdx > 0 && pollIdx % 100 == 0) {
					UINT32 paramSt = apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS);
					UINT32 hibErr  = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
					DbgPrint("[PARAM_CACHE@%dms] PARAM_POP=0x%x COMPLETED=%llu HIB=0x%x\n",
						pollIdx, paramSt, done, hibErr);
					if (hibErr != 0) {
						DbgPrint("[PARAM_CACHE] HIB error during param load, aborting\n");
						status = STATUS_UNSUCCESSFUL;
						break;
					}
				}
				{ LARGE_INTEGER d; d.QuadPart = -10000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); }
			}
			if (!NT_SUCCESS(status)) {
				UINT32 paramSt = apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS);
				UINT32 hibErr  = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
				DbgPrint("[PARAM_CACHE] FAILED: PARAM_POP=0x%x HIB=0x%x status=0x%x\n",
					paramSt, hibErr, status);
			}
		}

		// 4.5. Post-Phase-1: IQ interrupt status 클리어 + 전체 run control kRunning 재설정.
		// PARAMETER_CACHING bitstream이 AVDATA/OUTFEED 등을 0으로 만든다.
		// PrepareHardware와 동일한 순서로 모든 유닛을 재시작해 Phase 2 준비.
		if (NT_SUCCESS(status)) {
			DbgPrint("[PARAM_CACHE] Pre-reset: AVDATA=0x%x INFEED=0x%x OUTFEED=0x%x PARAM=0x%x\n",
				apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS));
			// IQ interrupt status 클리어 (write 0 to clear)
			apex_write_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS, 0);
			// All run controls → kRunning (1)
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
			DbgPrint("[PARAM_CACHE] Post-reset: AVDATA=0x%x INFEED=0x%x OUTFEED=0x%x IQ_INT=0x%llx\n",
				apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS),
				apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS));
		}

		// 5. Clear parameter PTEs
		WdfSpinLockAcquire(pDevContext->PageTableLock);
		for (i = 0; i < pageCount; i++)
			apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), 0);
		WdfSpinLockRelease(pDevContext->PageTableLock);

		// 6. Unlock parameter pages
		MmUnlockPages(paramMdl);
		IoFreeMdl(paramMdl);
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
