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

		// =========================================================
		// BITSTREAM PATCH VERIFICATION
		// Dump first 256 B of the INFER bitstream the chip will read,
		// then scan whole bitstream for the four patch values that
		// userspace claims it patched in (INPUT/OUTPUT[0]/OUTPUT[1]/PARAM).
		// If any are missing, OUTFEED/INFEED will write/read at chip
		// defaults (often 0) and we silently get a 0-output buffer.
		// pDevContext->LockedModelMdl tracks the most recent
		// IOCTL_MAP_BUFFER, which after PARAM_CACHE was the INFER
		// bitstream MAP — so it's exactly what we want here.
		// =========================================================
		if (pDevContext->LockedModelMdl != NULL) {
			PUCHAR bskva = (PUCHAR)MmGetSystemAddressForMdlSafe(
				pDevContext->LockedModelMdl, NormalPagePriority);
			ULONG bsLen = (ULONG)pDevContext->LockedModelSize;
			if (bskva != NULL) {
				DbgPrint("[BS-DUMP] kernel VA=%p size=%lu (0x%lx) deviceVA=0x%llx\n",
					bskva, bsLen, bsLen, pInput->BitstreamDeviceVA);
				DbgPrint("[BS-DUMP] [00] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					bskva[0],  bskva[1],  bskva[2],  bskva[3],  bskva[4],  bskva[5],  bskva[6],  bskva[7],
					bskva[8],  bskva[9],  bskva[10], bskva[11], bskva[12], bskva[13], bskva[14], bskva[15]);
				DbgPrint("[BS-DUMP] [10] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					bskva[16], bskva[17], bskva[18], bskva[19], bskva[20], bskva[21], bskva[22], bskva[23],
					bskva[24], bskva[25], bskva[26], bskva[27], bskva[28], bskva[29], bskva[30], bskva[31]);
				DbgPrint("[BS-DUMP] [20] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					bskva[32], bskva[33], bskva[34], bskva[35], bskva[36], bskva[37], bskva[38], bskva[39],
					bskva[40], bskva[41], bskva[42], bskva[43], bskva[44], bskva[45], bskva[46], bskva[47]);
				DbgPrint("[BS-DUMP] [30] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					bskva[48], bskva[49], bskva[50], bskva[51], bskva[52], bskva[53], bskva[54], bskva[55],
					bskva[56], bskva[57], bskva[58], bskva[59], bskva[60], bskva[61], bskva[62], bskva[63]);

				// Scan whole bitstream for 32-bit LE patch values.  Apex
				// patches are dword-aligned in the bitstream, so we step
				// in 4-byte units.  Report up to 4 hits per pattern.
				{
					UINT32 patterns[5] = {
						(UINT32)pInput->InputDeviceVA,   // 0x631000
						(UINT32)pInput->OutputDeviceVA,  // 0x67c000
						(UINT32)(pInput->OutputDeviceVA + 0x2000), // 0x67e000 (OUTPUT[1])
						0x3000u,                         // PARAM device VA
						(UINT32)pInput->BitstreamDeviceVA // 0x600000 self-base
					};
					const char *names[5] = {"INPUT", "OUTPUT[0]", "OUTPUT[1]", "PARAM", "BITSTREAM_SELF"};
					ULONG p, off, hits;
					for (p = 0; p < 5; p++) {
						hits = 0;
						for (off = 0; off + 4 <= bsLen; off += 4) {
							UINT32 v = *(UINT32 *)(bskva + off);
							if (v == patterns[p]) {
								if (hits < 4) {
									DbgPrint("[BS-SCAN]   %s=0x%x found at off=0x%lx\n",
										names[p], patterns[p], off);
								}
								hits++;
							}
						}
						DbgPrint("[BS-SCAN] %s 0x%08x: %lu hit(s)%s\n",
							names[p], patterns[p], hits,
							(hits == 0) ? "  *** NOT FOUND — patch missing? ***" : "");
					}
				}
			} else {
				DbgPrint("[BS-DUMP] MmGetSystemAddressForMdlSafe(LockedModelMdl) returned NULL\n");
			}
		} else {
			DbgPrint("[BS-DUMP] LockedModelMdl is NULL — bitstream MDL not retained, skipping verification\n");
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
		// Save device VAs / sizes for DPC-time diagnostics.
		pDevContext->InferOutputDeviceVA = pInput->OutputDeviceVA;
		pDevContext->InferOutputSize     = pInput->OutputBufferSize;
		pDevContext->InferInputDeviceVA  = pInput->InputDeviceVA;
		pDevContext->InferInputSize      = pInput->InputImageSize;
		// Reset cumulative ISR pending-bit log for this inference.
		pDevContext->IsrSeenPendingBits = 0;
		pDevContext->LastIsrWirePending = 0;
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

			// SENTINEL FILL: pre-fill the locked output pages with 0xCC so we
			// can tell after inference whether OUTFEED actually wrote anything.
			//   bytes still 0xCC after inference -> OUTFEED never wrote
			//   bytes are 0x00                    -> OUTFEED wrote zeros (real
			//                                        but useless inference output)
			//   bytes are something else          -> OUTFEED wrote real data
			{
				PUCHAR outKva = (PUCHAR)MmGetSystemAddressForMdlSafe(
					outputBufferMdl, NormalPagePriority);
				if (outKva != NULL) {
					RtlFillMemory(outKva, (SIZE_T)pInput->OutputBufferSize, 0xCC);
					DbgPrint("[%s] Output buffer pre-filled with 0xCC (size=%llu) at kVA=%p\n",
						__FUNCTION__, pInput->OutputBufferSize, outKva);
				} else {
					DbgPrint("[%s] WARNING: cannot map output MDL to kernel VA — sentinel skipped\n",
						__FUNCTION__);
				}
			}
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

		// === HIB credit dump (PRE-INFER-SUBMIT) — does descriptor submission
		// itself consume credits?  If credits drop to 0 here, OUTFEED can't
		// emit host writes during inference.
		{
			UINT32 c0 = apex_read_register_32(bar2, APEX_REG_HIB_INSTRUCTION_CREDITS);
			UINT32 c1 = apex_read_register_32(bar2, APEX_REG_HIB_INPUT_ACTV_CREDITS);
			UINT32 c2 = apex_read_register_32(bar2, APEX_REG_HIB_PARAM_CREDITS);
			UINT32 c3 = apex_read_register_32(bar2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
			DbgPrint("[CREDITS@PRE-INFER]   instr=0x%08x input=0x%08x param=0x%08x output=0x%08x\n",
				c0, c1, c2, c3);
		}

		// PCI Command/Status snapshot — confirms BME=1 and no Master Abort right
		// before we kick the descriptor.  If these flip during inference we'll
		// see it in the post-DONE/post-TIMEOUT snapshots below.
		npudriverDumpPciCommand(device, "pre-submit");
		npudriverDumpPciAer(device, "pre-submit");

		// AXI write credit shim counters — these are R/O statistics maintained by
		// the chip's internal AXI master.  By dumping pre-submit and post-DONE we
		// can determine if the chip even attempted outbound AXI writes during
		// inference.  Pre-INFER baseline (probably 0); post-INFER non-zero if the
		// chip emitted AW/W transactions (then we have a routing/PTE problem); if
		// they stay at 0 the chip never reached the AXI master phase at all.
		{
			UINT32 awIns = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION);
			UINT32 wIns  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_INSERTION);
			UINT32 awOcc = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY);
			UINT32 wOcc  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY);
			DbgPrint("[AXI@pre-submit] aw_insertion=0x%x w_insertion=0x%x aw_occupancy=0x%x w_occupancy=0x%x\n",
				awIns, wIns, awOcc, wOcc);
		}

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
			// Stage 1 expired — check inference completion.
			//
			// IMPORTANT: COMPLETED == TAIL alone is NOT sufficient.  IQ COMPLETED
			// advances when the chip *fetches* a descriptor and dispatches it to
			// the engines; SCALAR may still be executing the bitstream.  We saw
			// this empirically: ISR fired at 6 ms with COMPLETED=2 == TAIL=2 but
			// SCALAR/INFEED were still kRun(1).  Real "inference complete" is
			// SCALAR==kIdle(0) (or kHalted(4) on fault).
			UINT32 iqSize        = (UINT32)apex_read_register(bar2, APEX_REG_INSTR_QUEUE_SIZE);
			UINT32 expectedDone  = pDevContext->DescRingTail & (iqSize - 1);
			UINT32 scStatus50ms  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT64 completed50ms = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 hibErr50ms    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[50MS] SCALAR_RUN_STATUS=0x%x COMPLETED=0x%llx expected=%u TAIL=%u HIB_ERROR=0x%llx\n",
				scStatus50ms, completed50ms, expectedDone, pDevContext->DescRingTail, hibErr50ms);

			// Fast path: SCALAR already terminal (kIdle or kHalted) AND IQ done
			if ((UINT32)completed50ms == expectedDone &&
			    (scStatus50ms == 0 || scStatus50ms == 4)) {
				DbgPrint("[50MS] inference truly done (COMPLETED=%llu SC=0x%x)\n",
					completed50ms, scStatus50ms);
				status = STATUS_SUCCESS;
			}

			// Stage 2: poll until SCALAR is in {kIdle, kHalted} AND IQ done (max 5s)
			if (status == STATUS_TIMEOUT) {
				int pollIdx;
				for (pollIdx = 0; pollIdx < 1000; pollIdx++) {
					UINT64 done = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
					UINT32 sc   = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
					if ((UINT32)done == expectedDone && (sc == 0 || sc == 4)) {
						DbgPrint("[POLL] inference done after %d ms (COMPLETED=%llu SC=0x%x TAIL=%u)\n",
							50 + pollIdx, done, sc, pDevContext->DescRingTail);
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

				// Post-completion PCI snapshot — Master Abort here means chip
				// did issue an outbound write but root complex rejected it
				// (typically VT-d/IOMMU denying the target PA).
				npudriverDumpPciCommand(device, "post-DONE");
				npudriverDumpPciAer(device, "post-DONE");
				if (hibErr & 1ULL)
					DbgPrint("[DONE]   inbound_page_fault at device VA 0x%llx\n", firstErr);

				// AXI shim counters POST-INFER — compare with pre-submit snapshot.
				// Non-zero increment = chip attempted outbound writes (PTE/IOMMU bug).
				// Zero = chip's OUTFEED never reached AXI master (silicon-level gate).
				{
					UINT32 awIns = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION);
					UINT32 wIns  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_INSERTION);
					UINT32 awOcc = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY);
					UINT32 wOcc  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY);
					DbgPrint("[AXI@post-DONE] aw_insertion=0x%x w_insertion=0x%x aw_occupancy=0x%x w_occupancy=0x%x\n",
						awIns, wIns, awOcc, wOcc);
				}

				// HIB credit dump (POST-INFER) — did credits decrement during
				// inference?  Compare with PRE-INFER snapshot.  output==0 here
				// + PRE-INFER == 0 confirms outbound credit gate was the issue.
				{
					UINT32 c0 = apex_read_register_32(bar2, APEX_REG_HIB_INSTRUCTION_CREDITS);
					UINT32 c1 = apex_read_register_32(bar2, APEX_REG_HIB_INPUT_ACTV_CREDITS);
					UINT32 c2 = apex_read_register_32(bar2, APEX_REG_HIB_PARAM_CREDITS);
					UINT32 c3 = apex_read_register_32(bar2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
					DbgPrint("[CREDITS@POST-INFER]  instr=0x%08x input=0x%08x param=0x%08x output=0x%08x\n",
						c0, c1, c2, c3);
				}

				// Status block dump at completion time
				if (pDevContext->StatusBlockBase != NULL) {
					UINT64 *sb = (UINT64 *)pDevContext->StatusBlockBase;
					DbgPrint("[DONE-SB] StatusBlock: [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx\n",
						sb[0], sb[1], sb[2], sb[3]);
				}

				// =========================================================
				// SCAN — chip 이 OUTFEED 로 발행한 ~2585 burst 가 host 의 어느
				// PA 에 도착했는지 추적.  우리가 PTE 에 등록한 모든 영역의 head/tail
				// 64 bytes 를 dump.  추론 결과처럼 보이는 byte 패턴이 어느 영역에서
				// 나타나는지로 chip 의 진짜 destination PA 를 역추적한다.
				//
				// 참고 패턴:
				//   0xCC repeated      → 우리가 Output 에 pre-fill 한 sentinel.  변화 없음.
				//   0x80 0f 00 10 c3.. → bitstream header (PARAM/INFER bitstream 영역)
				//   0x251B31...        → image pixels (Input 영역, 로그에서 본 첫 byte)
				//   StatusBlock [0]=0x2 → IQ completed_head, chip 이 정상 write 한 케이스
				//   기타 small float / quantized int8 → OUTFEED 결과 가능성
				// =========================================================
				DbgPrint("[SCAN] post-inference dump of all PTE-mapped regions\n");

				// 1) DescRing — kernel allocated, 4 KB.  inference 전 첫 32 bytes 는
				//    우리가 채운 descriptor (address+size) 였음.  chip 이 여기를
				//    덮었는지 확인.
				if (pDevContext->DescRingBase != NULL) {
					PUCHAR p = (PUCHAR)pDevContext->DescRingBase;
					DbgPrint("[SCAN-DescRing] kva=%p PA=0x%llx first32=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						p, pDevContext->DescRingDeviceVA,
						p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
						p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
						p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
						p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31]);
				}

				// 2) StatusBlock — kernel allocated, 4 KB.  처음 16 bytes 외에 나머지
				//    영역도 dump (chip 이 잘못 변환된 VA 로 여기를 덮었을 가능성).
				if (pDevContext->StatusBlockBase != NULL) {
					PUCHAR p = (PUCHAR)pDevContext->StatusBlockBase;
					DbgPrint("[SCAN-StatusBlock] kva=%p first32=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						p,
						p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
						p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
						p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
						p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31]);
				}

				// 3) PageTable host-side memory — 우리가 PTE 들을 작성한 host buffer.
				//    chip 이 만약 device VA 의 high bits 를 잘못 해석해서 PTE
				//    memory 영역으로 결과를 썼을 가능성.  64 KB 영역의 시작.
				if (pDevContext->PageTableBase != NULL) {
					PUCHAR p = (PUCHAR)pDevContext->PageTableBase;
					DbgPrint("[SCAN-PageTable] kva=%p first32=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						p,
						p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
						p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
						p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
						p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31]);
				}

				// 4) INFER bitstream MDL (LockedModelMdl) — 196 KB at PTE[1536..1584].
				//    inference 전 bitstream header 는 80 0f 00 10 c3 00 00 00.
				//    head 변하면 chip 이 bitstream 자체를 덮은 것.
				if (pDevContext->LockedModelMdl != NULL) {
					PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
						pDevContext->LockedModelMdl, NormalPagePriority);
					if (kva != NULL) {
						SIZE_T sz = MmGetMdlByteCount(pDevContext->LockedModelMdl);
						DbgPrint("[SCAN-INFERBitstream] kva=%p sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							kva, (UINT64)sz,
							kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
							kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
							kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
							kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
					}
				}

				// 5) Input image MDL — 300 KB at PTE[1585..1659].  inference 전
				//    image pixels (head 0x25 0x1B...).  chip 이 OUTFEED 로 input
				//    영역을 덮으면 head 가 무의미한 byte 로 변함.
				if (pDevContext->InferInputMdl != NULL) {
					PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
						pDevContext->InferInputMdl, NormalPagePriority);
					if (kva != NULL) {
						SIZE_T sz = MmGetMdlByteCount(pDevContext->InferInputMdl);
						DbgPrint("[SCAN-InputImage] kva=%p sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							kva, (UINT64)sz,
							kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
							kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
							kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
							kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
					}
				}

				// 6) Cached PARAM bitstream MDL — 12 KB at PTE[0..2], device VA 0x0.
				if (pDevContext->CachedParamBitstreamMdl != NULL) {
					PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
						pDevContext->CachedParamBitstreamMdl, NormalPagePriority);
					if (kva != NULL) {
						SIZE_T sz = MmGetMdlByteCount(pDevContext->CachedParamBitstreamMdl);
						DbgPrint("[SCAN-PARAMBitstream] kva=%p sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							kva, (UINT64)sz,
							kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
							kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
							kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
							kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
					}
				}

				// 7) Cached Param data MDL — 6 MB at PTE[3..1502], device VA 0x3000.
				//    너무 크니까 head 만.
				if (pDevContext->CachedParamMdl != NULL) {
					PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
						pDevContext->CachedParamMdl, NormalPagePriority);
					if (kva != NULL) {
						SIZE_T sz = MmGetMdlByteCount(pDevContext->CachedParamMdl);
						DbgPrint("[SCAN-ParamData] kva=%p sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | @+0x67c000 (=0x67c000-0x3000=0x679000): %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							kva, (UINT64)sz,
							kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
							kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
							/* 만약 chip 이 0x67c000 device VA 를 PARAM 영역의 offset 으로 잘못
							   해석했다면 여기 */
							sz > 0x679000 ? kva[0x679000] : 0, sz > 0x679001 ? kva[0x679001] : 0,
							sz > 0x679002 ? kva[0x679002] : 0, sz > 0x679003 ? kva[0x679003] : 0,
							sz > 0x679004 ? kva[0x679004] : 0, sz > 0x679005 ? kva[0x679005] : 0,
							sz > 0x679006 ? kva[0x679006] : 0, sz > 0x679007 ? kva[0x679007] : 0,
							sz > 0x679008 ? kva[0x679008] : 0, sz > 0x679009 ? kva[0x679009] : 0,
							sz > 0x67900a ? kva[0x67900a] : 0, sz > 0x67900b ? kva[0x67900b] : 0,
							sz > 0x67900c ? kva[0x67900c] : 0, sz > 0x67900d ? kva[0x67900d] : 0,
							sz > 0x67900e ? kva[0x67900e] : 0, sz > 0x67900f ? kva[0x67900f] : 0);
					}
				}

				// Output buffer kernel-side dump BEFORE unlock — catches the case
				// where chip claims SC==kIdle but OUTFEED never actually wrote.
				if (pDevContext->InferOutputMdl != NULL) {
					PMDL outMdl    = pDevContext->InferOutputMdl;
					UINT64 outVA   = pDevContext->InferOutputDeviceVA;
					UINT64 outSize = pDevContext->InferOutputSize;
					UINT32 outPteIdx = (UINT32)(outVA >> PAGE_SHIFT);
					UINT32 outPageCnt = (UINT32)((outSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
					PPFN_NUMBER outPfn = MmGetMdlPfnArray(outMdl);

					// Verify PTE readback for first up-to-4 output pages.
					{
						UINT32 vc = (outPageCnt < 4) ? outPageCnt : 4;
						UINT32 ii;
						for (ii = 0; ii < vc; ii++) {
							UINT64 expectPA  = (UINT64)outPfn[ii] << PAGE_SHIFT;
							UINT64 readPA    = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + ((outPteIdx + ii) * 8));
							UINT64 readPaNoF = readPA & ~1ULL;
							DbgPrint("[DONE-PTE] OUTPUT PTE[%u+%u] expect=0x%llx read=0x%llx %s\n",
								outPteIdx, ii, expectPA, readPaNoF,
								(readPaNoF == expectPA) ? "OK" : "MISMATCH");
						}
					}

					// Map locked output pages, scan first 16 KB for non-zero, dump 64B.
					{
						PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(outMdl, NormalPagePriority);
						if (kva != NULL) {
							ULONG dumpLen = (outSize < 64) ? (ULONG)outSize : 64;
							ULONG scanLen = (outSize < 0x4000) ? (ULONG)outSize : 0x4000;
							ULONG j;
							ULONG nz = 0;
							for (j = 0; j < scanLen; j++) {
								if (kva[j] != 0) nz++;
							}
							DbgPrint("[DONE-OUT] kernel VA=%p outVA=0x%llx size=%llu nonzero(first %lu B)=%lu\n",
								kva, outVA, outSize, scanLen, nz);
							DbgPrint("[DONE-OUT] [00] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
								kva[0],  kva[1],  kva[2],  kva[3],  kva[4],  kva[5],  kva[6],  kva[7],
								kva[8],  kva[9],  kva[10], kva[11], kva[12], kva[13], kva[14], kva[15]);
							if (dumpLen > 16) {
								DbgPrint("[DONE-OUT] [10] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
									kva[16], kva[17], kva[18], kva[19], kva[20], kva[21], kva[22], kva[23],
									kva[24], kva[25], kva[26], kva[27], kva[28], kva[29], kva[30], kva[31]);
							}
							// Also dump near OUTPUT[1] (device VA 0x67e000 = +0x2000 = +8192 bytes)
							if (outSize >= 0x2010) {
								DbgPrint("[DONE-OUT] [@+0x2000 OUTPUT[1] start] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
									kva[0x2000], kva[0x2001], kva[0x2002], kva[0x2003],
									kva[0x2004], kva[0x2005], kva[0x2006], kva[0x2007],
									kva[0x2008], kva[0x2009], kva[0x200a], kva[0x200b],
									kva[0x200c], kva[0x200d], kva[0x200e], kva[0x200f]);
							}
						} else {
							DbgPrint("[DONE-OUT] MmGetSystemAddressForMdlSafe returned NULL\n");
						}
					}
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

			// Post-timeout PCI snapshot — same intent as post-DONE.
			npudriverDumpPciCommand(device, "post-TIMEOUT");
			npudriverDumpPciAer(device, "post-TIMEOUT");

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
