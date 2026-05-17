#include "Driver.h"
#include "Queue.h"
#include "Memory.h"
#include <ntstrsafe.h>  // RtlStringCbPrintfA/W for ApexDumpCsrRegions file output


#ifdef ALLOC_PRAGMA
// IOCTL 핸들러는 spinlock을 잡거나 (PageTableLock) MMIO와 즉결 처리가 필요하므로
// NONPAGED로 둔다. PAGED 로 두면 lock 안에서의 DbgPrint 등 정적 데이터 접근이
// DISPATCH_LEVEL 에서 page fault -> D3 BSOD 를 일으킨다.
//#pragma alloc_text(PAGE, npudriverEvtIoDeviceControl)
#endif

// === DIAGNOSTIC: file I/O isolation test (2026-05-16) =======================
// Tests ZwCreateFile/ZwWriteFile only — NO MMIO. If this crashes, file I/O
// is the BSOD culprit. If 3 files appear in C:\temp\test_<tag>.log, file I/O
// works fine and the previous crash was elsewhere (MMIO loop most likely).
// Phase markers via DbgPrint locate the exact failure phase.
static VOID TestFileWrite(const char* tag) {
    DbgPrint("[TEST@%s] P1: enter, IRQL=%u\n", tag, (unsigned)KeGetCurrentIrql());

    WCHAR pathBuf[256];
    NTSTATUS st = RtlStringCbPrintfW(pathBuf, sizeof(pathBuf),
        L"\\??\\C:\\temp\\test_%hs.log", tag);
    DbgPrint("[TEST@%s] P2: path build status=0x%08x\n", tag, st);

    UNICODE_STRING fp;
    RtlInitUnicodeString(&fp, pathBuf);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &fp,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    DbgPrint("[TEST@%s] P3: about to ZwCreateFile\n", tag);

    HANDLE hFile = NULL;
    IO_STATUS_BLOCK iosb;
    st = ZwCreateFile(&hFile, GENERIC_WRITE | SYNCHRONIZE,
        &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0);
    DbgPrint("[TEST@%s] P4: ZwCreateFile returned 0x%08x (hFile=%p)\n",
        tag, st, hFile);

    if (NT_SUCCESS(st)) {
        char msg[128];
        size_t msgLen;
        RtlStringCbPrintfA(msg, sizeof(msg),
            "hello from npu_driver, tag=%s\r\n", tag);
        RtlStringCbLengthA(msg, sizeof(msg), &msgLen);
        DbgPrint("[TEST@%s] P5: about to ZwWriteFile (%zu bytes)\n", tag, msgLen);

        st = ZwWriteFile(hFile, NULL, NULL, NULL, &iosb,
            msg, (ULONG)msgLen, NULL, NULL);
        DbgPrint("[TEST@%s] P6: ZwWriteFile returned 0x%08x (info=%llu)\n",
            tag, st, (UINT64)iosb.Information);

        ZwClose(hFile);
        DbgPrint("[TEST@%s] P7: ZwClose done\n", tag);
    }
    DbgPrint("[TEST@%s] P8: exit\n", tag);
}

// === BAR2 CSR region dump helper (writes to C:\temp\npu_csr_<tag>.log) =======
// BISECTION VERSION 2026-05-16: restricted to 4 KNOWN-SAFE regions
// (the original set libedgetpu has been reading for years without issue).
// Previous full 0x00000-0x80000 sweep crashed system — narrowed scope here
// to confirm file I/O + 4-region MMIO works. Once confirmed safe, add new
// regions ONE AT A TIME via the kRegions table to find which one crashes.
//
// Writes to file instead of DbgPrint (DbgPrint is too slow for 65k lines).
// File: \??\C:\temp\npu_csr_<tag>.log (overwrite-if-exists).
// Format: matches libedgetpu `[CSR@<tag>] off=0x... val=0x...` for diff.
static VOID ApexDumpCsrRegions(PDEVICE_CONTEXT pDc, const char* tag) {
    PVOID bar2 = pDc->Bar2BaseAddress;
    if (bar2 == NULL || pDc->Bar2Length == 0) {
        DbgPrint("[CSR@%s] SKIP: BAR2 not mapped\n", tag);
        return;
    }
    if (KeGetCurrentIrql() > APC_LEVEL) {
        DbgPrint("[CSR@%s] SKIP: IRQL=%u too high for file I/O\n",
            tag, (unsigned)KeGetCurrentIrql());
        return;
    }

    // Known-safe regions only. To bisect new regions, add an entry here one at a time and re-test.
    static const struct {
        UINT64 off;
        UINT64 size;
        const char* name;
    } kRegions[] = {
        { 0x40000, 0x0400, "scalar_core"        },  // 128 entries
        { 0x42000, 0x2000, "tile_array"         },  // 1024 entries — NEW BISECTION CANDIDATE 2026-05-16
        { 0x44000, 0x0400, "data_feed_control"  },  // 128 entries
        { 0x46000, 0x0100, "hib_kernel"         },  //  32 entries
        { 0x48000, 0x0800, "hib_user"           },  // 256 entries
        { 0x1A000, 0x0340, "scu_omc"            },  // 104 entries — ADDED 2026-05-16 (5x throughput hunt)
        // {0x50000, 0x10000, "page_table"      },  // candidate next
    };
    const ULONG regionCount = sizeof(kRegions) / sizeof(kRegions[0]);

    // 512KB buffer is plenty for the 4-region (~16KB) baseline.
    const SIZE_T kBufSize = 512ULL * 1024;
    PCHAR buf = (PCHAR)ExAllocatePoolWithTag(PagedPool, kBufSize, 'CSRD');
    if (!buf) {
        DbgPrint("[CSR@%s] SKIP: ExAllocatePoolWithTag(512KB) failed\n", tag);
        return;
    }

    PCHAR pos = buf;
    SIZE_T remaining = kBufSize;
    size_t lineLen;

    RtlStringCbPrintfA(pos, remaining,
        "[CSR-BEGIN@%s] %lu regions (bisection baseline)\n", tag, regionCount);
    lineLen = strlen(pos);
    pos += lineLen; remaining -= lineLen;

    ULONG r;
    for (r = 0; r < regionCount; r++) {
        UINT64 base = kRegions[r].off;
        UINT64 size = kRegions[r].size;
        if (base + size > (UINT64)pDc->Bar2Length) {
            RtlStringCbPrintfA(pos, remaining,
                "[CSR@%s] --- region %s 0x%05llx..0x%05llx SKIPPED (past Bar2Length=0x%lx) ---\n",
                tag, kRegions[r].name, base, base + size - 1, pDc->Bar2Length);
            lineLen = strlen(pos);
            pos += lineLen; remaining -= lineLen;
            continue;
        }
        RtlStringCbPrintfA(pos, remaining,
            "[CSR@%s] --- region %s 0x%05llx..0x%05llx ---\n",
            tag, kRegions[r].name, base, base + size - 1);
        lineLen = strlen(pos);
        pos += lineLen; remaining -= lineLen;

        UINT64 o;
        for (o = 0; o < size; o += 8) {
            UINT64 abs = base + o;
            UINT64 val = apex_read_register(bar2, abs);
            RtlStringCbPrintfA(pos, remaining,
                "[CSR@%s] off=0x%05llx val=0x%016llx\n", tag, abs, val);
            lineLen = strlen(pos);
            pos += lineLen; remaining -= lineLen;
        }
    }

    RtlStringCbPrintfA(pos, remaining, "[CSR-END@%s]\n", tag);
    lineLen = strlen(pos);
    pos += lineLen; remaining -= lineLen;

    SIZE_T totalLen = (SIZE_T)(pos - buf);

    // Build file path: \??\C:\temp\npu_csr_<tag>.log
    WCHAR pathBuf[256];
    RtlStringCbPrintfW(pathBuf, sizeof(pathBuf),
        L"\\??\\C:\\temp\\npu_csr_%hs.log", tag);
    UNICODE_STRING filePath;
    RtlInitUnicodeString(&filePath, pathBuf);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &filePath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    HANDLE hFile = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = ZwCreateFile(&hFile, GENERIC_WRITE | SYNCHRONIZE,
        &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0);
    if (NT_SUCCESS(st)) {
        st = ZwWriteFile(hFile, NULL, NULL, NULL, &iosb,
            buf, (ULONG)totalLen, NULL, NULL);
        ZwClose(hFile);
        DbgPrint("[CSR@%s] wrote %llu bytes to C:\\temp\\npu_csr_%s.log (status=0x%08x)\n",
            tag, (UINT64)totalLen, tag, st);
    } else {
        DbgPrint("[CSR@%s] ZwCreateFile failed status=0x%08x — does C:\\temp exist?\n",
            tag, st);
    }

    ExFreePoolWithTag(buf, 'CSRD');
}

VOID arm_tile_and_engiend(void* bar2, PDEVICE_CONTEXT pDc) {
	apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
	int tile_poll;
	for (tile_poll = 0; tile_poll < 1000; tile_poll++) {
		UINT64 v = apex_read_register(bar2, APEX_REG_TILE_CONFIG0);
		if (v == 0x7F) {
			DbgPrint("[INFER_NEW] | tile_polling | SUCCESS\n");
			break;
		}
		KeStallExecutionProcessor(10);
	}

	if (tile_poll == 1000) {
		DbgPrint("[INFER_NEW] TILE_CONFIG0 broadcast 미수렴: last=0x%llx\n",
			apex_read_register(bar2, APEX_REG_TILE_CONFIG0));
	}

	// 왜 한줄에 하나씩 전부 키는건가? 
	// Edge TPU 데이터패스는 파이프라인된 독립 엔진들의 집합이다. 각자 자기 명령 큐를 fetch해서 실행하므로, 
	// 하나라도 Halted면 그 단계에서 파이프라인이 막힌다. 
	//																		// 엔진 종류			
	apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);			// Scalar Core, 제어 흐름 / 주소 계산 담당 스칼라 프로세서 기동
	apex_write_register(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL, 1);		// Activation	activation 데이터를 narrow memory -> 연산 유닛으로 push
	apex_write_register(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1);	// 가중치(Weights)를 parameter memory -> MAC array로 push
	apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);			// host -> chip 입력 데이터 DMA 엔진 기동
	apex_write_register(bar2, APEX_REG_OUTFEED_RUN_CONTROL, 1);			// chip -> host 출력 데이터 dma 엔진 기동
	apex_write_register(bar2, APEX_REG_TILE_OP_RUN_CONTROL, 1);			// MAC array 연산 명령 디스패처, 실제 compute 수행
	apex_write_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL, 1);	// narrow(int8) -> wide(int32 accumulator) 폭 변환 버스
	apex_write_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL, 1);	// wide(int32 acc) -> narrow(int8 quantized) 폭 변환 버스
	apex_write_register(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
	apex_write_register(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
	apex_write_register(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
	apex_write_register(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
	apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);	// 호스트가 ring으로 보낸 명령 소비 엔진 #0
	apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);	// 호스트가 ring으로 보낸 명령 소비 엔진 #1
	apex_write_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL, 1);	// 칩이 호스트로 완료/응답을 보내는 ring 송신 엔진
	KeStallExecutionProcessor(1000); // 1ms settle

	if (pDc->StatusBlockBase != NULL) {
		PUCHAR base = (PUCHAR)pDc->StatusBlockBase;
		DbgPrint("[INFER_NEW] | [test debug] | before desc submit | %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
			base[0], base[1], base[2], base[3], base[4], base[5], base[6], base[7],
			base[8], base[9], base[10], base[11], base[12], base[13], base[14], base[15]);
	}
}

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
			pDC->LastMapBufferDeviceVA = deviceAddr;
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
		break;
	case IOCTL_INFER_NEW:	// direct typing 
	{
		DbgPrint("IOCTL_INFER_NEW Start!\n");
		PDEVICE_CONTEXT pDc = DeviceGetContext(device);
		PVOID bar2 = pDc->Bar2BaseAddress;
		WDFMEMORY inMem;
		IOCTL_INFER_INFO* pIn = NULL;
		ALLOC_IO_SLOT* inSlot = NULL, * outSlot = NULL, * scSlot = NULL;
		UINT32 i;

		// infeed가 바라보는 실제 값이 존재하는지 체크 
		PUCHAR inputKva = (PUCHAR)pDc->IOSlots[0].Kva;
		SIZE_T inputSize = pDc->IOSlots[0].Size;
		if (inputKva != NULL && inputSize > 0) {
			DbgPrint("[INFER_NEW] Infeed Data (First 16 bytes): \n");
			DbgPrint("%02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
				inputKva[0], inputKva[1], inputKva[2], inputKva[3],
				inputKva[4], inputKva[5], inputKva[6], inputKva[7],
				inputKva[8], inputKva[9], inputKva[10], inputKva[11],
				inputKva[12], inputKva[13], inputKva[14], inputKva[15]);
		}
		
		// param data va check 
		PUCHAR pParam = (PUCHAR)pDc->IOSlots[IO_SLOT_PARAM_DATA].Kva;
		DbgPrint("[INFER NEW] | PARAM DATA Check\n");
		size_t pIdx;
		for (pIdx = 0; pIdx < 32; ++pIdx) DbgPrint(" %02x", pParam[pIdx]);
		DbgPrint("\n");

		// [1] retrieve
		if (InputBufferLength < sizeof(IOCTL_INFER_INFO)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		status = WdfRequestRetrieveInputMemory(Request, &inMem);
		if (!NT_SUCCESS(status)) break;
		pIn = (IOCTL_INFER_INFO*)WdfMemoryGetBuffer(inMem, NULL);

		// simple VA 만 - extended bit 셋이면 즉시 거부 
		if ((pIn->InputDeviceVA & (1ULL << 63)) ||	// 64 비트가 1이면 extended 영역이라서 그렇게 하는거 .. 
			(pIn->OutputDeviceVA & (1ULL << 63)) ||
			((pIn->ScratchSize > 0) && (pIn->ScratchDeviceVA & (1ULL << 63)))) {
			DbgPrint("[INFER_NEW] extended VA not supported in this path\n");
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// [2] IoSlots lookup - IOCTL_ALLOC_IO_BUFFERS 가 잡아둔 슬롯 셋을 찾는다.
		// lock 안함
		// MDL 안만듦
		// pte 안박음
		for (i = 0; i < IO_SLOT_COUNT; i++) {
			ALLOC_IO_SLOT* s = &pDc->IOSlots[i];
			if (s->Kva == NULL) continue;
			if ((UINT64)s->UserVa == pIn->InputImageAddr && s->DeviceVa == pIn->InputDeviceVA) inSlot = s;
			if ((UINT64)s->UserVa == pIn->OutputBufferAddr && s->DeviceVa == pIn->OutputDeviceVA) outSlot = s;
			if (pIn->ScratchSize > 0 && 
				(UINT64)s->UserVa == pIn->ScratchAddr && s->DeviceVa == pIn->ScratchDeviceVA) scSlot = s;
		}
		if (!inSlot || !outSlot || (pIn->ScratchSize > 0 && !scSlot)) {
			DbgPrint("[INFER_NEW] caller buffer not registered via IOCTL_ALLOC_IO_BUFFERS "
				"(in=%p out=%p sc=%p)\n", inSlot, outSlot, scSlot);
			status = STATUS_INVALID_DEVICE_STATE;
			break;
		}

		// ============================================================================
		// BS-FULL DUMP — bitstream 전체 hex dump + patch placeholder 스캔
		//   2384 byte 짜리 identity 모델 정도면 통째 dump해도 KD 콘솔이 감당 가능.
		//   목적: bitstream 안에 OUTFEED destination VA가 0x2000(우리 output)으로 박혔는지,
		//         아니면 placeholder/엉뚱한 VA가 박혔는지 확인.
		//   토글: BS_FULL_DUMP 1→0 으로 끄기.
		// ============================================================================
		#define BS_FULL_DUMP 0
		#if BS_FULL_DUMP
		do {
			ALLOC_IO_SLOT* bsSlot = &pDc->IOSlots[IO_SLOT_EXE0_BS];
			if (bsSlot->Kva == NULL) {
				DbgPrint("[BS-FULL] EXE0_BS slot empty — skipping bitstream dump\n");
				break;
			}

			SIZE_T claimed = (SIZE_T)pIn->BitstreamSize;
			SIZE_T slotSz = bsSlot->Size;
			SIZE_T sz = claimed;
			if (sz == 0 || sz > slotSz) sz = slotSz;

			DbgPrint("[BS-FULL] BitstreamDeviceVA=0x%llx claimed_size=0x%llx (%llu B) "
				"slot_size=0x%llx | InputVA=0x%llx OutputVA=0x%llx ScratchVA=0x%llx ScratchSize=0x%llx\n",
				pIn->BitstreamDeviceVA, (UINT64)claimed, (UINT64)claimed,
				(UINT64)slotSz,
				pIn->InputDeviceVA, pIn->OutputDeviceVA,
				pIn->ScratchDeviceVA, pIn->ScratchSize);

			// 16 byte/줄 hex dump — tail 잔여(< 16 byte)는 zero-pad 해서 같은 포맷으로 출력.
			// slot은 4KB 잡혀 있어서 sz를 16 byte 정렬 위로 round-up 해도 over-read 안 남.
			PUCHAR bs = (PUCHAR)bsSlot->Kva;
			SIZE_T szPadded = (sz + 15) & ~(SIZE_T)15;
			SIZE_T bsOff;
			for (bsOff = 0; bsOff < szPadded; bsOff += 16) {
				DbgPrint("[BS-FULL] %04llx: %02x %02x %02x %02x %02x %02x %02x %02x  "
					"%02x %02x %02x %02x %02x %02x %02x %02x%s\n",
					(UINT64)bsOff,
					bs[bsOff+0],bs[bsOff+1],bs[bsOff+2],bs[bsOff+3],
					bs[bsOff+4],bs[bsOff+5],bs[bsOff+6],bs[bsOff+7],
					bs[bsOff+8],bs[bsOff+9],bs[bsOff+10],bs[bsOff+11],
					bs[bsOff+12],bs[bsOff+13],bs[bsOff+14],bs[bsOff+15],
					(bsOff + 16 > sz) ? "  (incl. post-claimed bytes)" : "");
			}

			// 32-bit LE 스캔 — 입출력 VA / 일반 placeholder 패턴 / 의심 영역 매칭
			DbgPrint("[BS-SCAN] looking for InputVA=0x%llx OutputVA=0x%llx ScratchVA=0x%llx "
				"+ placeholders 0xDEADBEEF / 0xCAFEBABE / 0xABADCAFE / 0x00000000 "
				"+ IQ/SB area (0x1000000~0x1002000)\n",
				pIn->InputDeviceVA, pIn->OutputDeviceVA, pIn->ScratchDeviceVA);

			PUINT32 dw = (PUINT32)bs;
			SIZE_T dwords = sz / 4;
			SIZE_T k;
			ULONG hit_in = 0, hit_out = 0, hit_sc = 0, hit_magic = 0, hit_iq = 0;
			for (k = 0; k < dwords; ++k) {
				UINT32 v = dw[k];
				const char* tag = NULL;
				if (v == (UINT32)pIn->InputDeviceVA  && pIn->InputDeviceVA  != 0) { tag = "INPUT_VA"; hit_in++; }
				else if (v == (UINT32)pIn->OutputDeviceVA && pIn->OutputDeviceVA != 0) { tag = "OUTPUT_VA"; hit_out++; }
				else if (v == (UINT32)pIn->ScratchDeviceVA && pIn->ScratchDeviceVA != 0) { tag = "SCRATCH_VA"; hit_sc++; }
				else if (v == 0xDEADBEEFu) { tag = "DEADBEEF"; hit_magic++; }
				else if (v == 0xCAFEBABEu) { tag = "CAFEBABE"; hit_magic++; }
				else if (v == 0xABADCAFEu) { tag = "ABADCAFE"; hit_magic++; }
				else if (v >= 0x01000000u && v < 0x01002000u) { tag = "IQ/SB_AREA"; hit_iq++; }
				// 기존 if-else 체인 끝에 한 가지 더:
				else if (v != 0 && v < 0x01000000u && (v & 0xFFFu) == 0) {
					// page-aligned, < 16MB (simple PT 영역) — VA 후보
					tag = "VA?";
				}

				if (tag) {
					DbgPrint("[BS-SCAN] +0x%04llx: 0x%08x  (%s)\n",
						(UINT64)(k*4), v, tag);
				}
			}

			DbgPrint("[BS-SCAN] hits: input=%u output=%u scratch=%u magic=%u iq_area=%u\n",
				hit_in, hit_out, hit_sc, hit_magic, hit_iq);

			// 진단 힌트
			if (hit_out == 0 && pIn->OutputDeviceVA != 0) {
				DbgPrint("[BS-SCAN] !!! OutputDeviceVA(0x%llx)가 bitstream에 한 번도 안 나타남 — "
					"compiler가 다른 VA를 hardcode했거나 patch 단계가 누락됨\n",
					pIn->OutputDeviceVA);
			}
			if (hit_in == 0 && pIn->InputDeviceVA != 0) {
				DbgPrint("[BS-SCAN] !!! InputDeviceVA(0x%llx)가 bitstream에 한 번도 안 나타남 — "
					"input도 patch 누락 또는 bitstream이 다른 VA 기대\n",
					pIn->InputDeviceVA);
			}
		} while (0);
		#endif

		// [3] 완료 이벤트 reset + 모든 engine kRun
		// tile_config0 도 다시 박아준다. (engine 이 RUN_CONTROL 거부 방지).
		KeClearEvent(&pDc->InferCompleteEvent);
		pDc->IsrSeenPendingBits = 0;
		
		apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
		int tile_poll;
		for (tile_poll = 0; tile_poll < 1000; tile_poll++) {
			UINT64 v = apex_read_register(bar2, APEX_REG_TILE_CONFIG0);
			if (v == 0x7F) {
				DbgPrint("[INFER_NEW] | tile_polling | SUCCESS\n");
				break;
			}
			KeStallExecutionProcessor(10);
		}

		if (tile_poll == 1000) {
			DbgPrint("[INFER_NEW] TILE_CONFIG0 broadcast 미수렴: last=0x%llx\n",
				apex_read_register(bar2, APEX_REG_TILE_CONFIG0));
		}
		
		// === EXPERIMENT 2026-05-16: per-IOCTL engine re-arm 비활성화 ===
		// libedgetpu는 DoOpen → DoRunControl(kMoveToRun) 에서 init 시 1회만 RUN_CONTROL=1.
		// user는 매 IOCTL_INFER_NEW 마다 15개 재무장 → BeforeSubmit CSR diff에서
		// scalar_core 엔진 status 블록 ~30개 offset divergence 유발 (0x400C8 saturated 등).
		// 가설: 이 재무장이 score-branch tile 의 잘못된 state 를 만든다.
		// 검증: 이 블록 비활성화 후 convert_scores 가 `ff 00 80 80` 에서 바뀌면 원인 확정.
		// 되돌리려면 `#if 0` → `#if 1`. init 시 arm 은 Device.c:757-792 가 담당.
#if 0
		// 왜 한줄에 하나씩 전부 키는건가?
		// Edge TPU 데이터패스는 파이프라인된 독립 엔진들의 집합이다. 각자 자기 명령 큐를 fetch해서 실행하므로,
		// 하나라도 Halted면 그 단계에서 파이프라인이 막힌다.
		//																		// 엔진 종류
		apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);			// Scalar Core, 제어 흐름 / 주소 계산 담당 스칼라 프로세서 기동
		apex_write_register(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL, 1);		// Activation	activation 데이터를 narrow memory -> 연산 유닛으로 push
		apex_write_register(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1);	// 가중치(Weights)를 parameter memory -> MAC array로 push
		apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);			// host -> chip 입력 데이터 DMA 엔진 기동
		apex_write_register(bar2, APEX_REG_OUTFEED_RUN_CONTROL, 1);			// chip -> host 출력 데이터 dma 엔진 기동
		apex_write_register(bar2, APEX_REG_TILE_OP_RUN_CONTROL, 1);			// MAC array 연산 명령 디스패처, 실제 compute 수행
		apex_write_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL, 1);	// narrow(int8) -> wide(int32 accumulator) 폭 변환 버스
		apex_write_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL, 1);	// wide(int32 acc) -> narrow(int8 quantized) 폭 변환 버스
		apex_write_register(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
		apex_write_register(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
		apex_write_register(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
		apex_write_register(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL, 1);		// 타일 간 mesh interconnect - 한 방향 라우터
		apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);	// 호스트가 ring으로 보낸 명령 소비 엔진 #0
		apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);	// 호스트가 ring으로 보낸 명령 소비 엔진 #1
		apex_write_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL, 1);	// 칩이 호스트로 완료/응답을 보내는 ring 송신 엔진
		KeStallExecutionProcessor(1000); // 1ms settle
#else
		DbgPrint("[INFER_NEW] EXPERIMENT: per-IOCTL RUN_CONTROL re-arm SKIPPED (rely on init-time arm in Device.c)\n");
#endif

		if (pDc->StatusBlockBase != NULL) {
			PUCHAR base = (PUCHAR)pDc->StatusBlockBase;
			DbgPrint("[INFER_NEW] | [test debug] | before desc submit | %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
				base[0], base[1], base[2], base[3], base[4], base[5], base[6], base[7],
				base[8], base[9], base[10], base[11], base[12], base[13], base[14], base[15]);
		}

		// input pte 검증 
		/*{
			DbgPrint("[INFER_NEW] | [CHECK] PTE readback for INPUT range:\n");
			size_t input_i;
			for (input_i = 0; input_i <= 75; input_i++) {
				UINT64 pte = apex_read_register(bar2, APEX_REG_PAGE_TABLE + input_i * 8);
				DbgPrint("  PTE[%2u] = 0x%llx  %s\n", input_i, pte, (pte & 1) ? "valid" : "INVALID");
			}
		}*/

		// [4] descriptor submit (single INFER, no PARAM)
		{
			DbgPrint("[before-EXE1] page_table_size=0x%llx extended=0x%llx translation_en=0x%llx\n",
				apex_read_register(bar2, APEX_REG_PAGE_TABLE_SIZE),
				apex_read_register(bar2, APEX_REG_EXTENDED_TABLE),
				apex_read_register(bar2, 0x46010));

			// PTE[15] 직접 read (slot 0 input의 fault VA 위치)
			DbgPrint("[before-EXE1] PTE[15] = 0x%llx (should be PA|0x1)\n",
				apex_read_register(bar2, APEX_REG_PAGE_TABLE + 15 * 8));   // 정확한 register 매크로 확인 필요

			// HIB_ERROR_MASK 도 확인 (어떤 에러를 활성화하는지 바뀌었는지)
			DbgPrint("[before-EXE1] hib_error_mask=0x%llx\n",

				apex_read_register(bar2, 0x486f8));

			// === Phase-1 진단 #2: PTE coverage 검증 ===
			// 모든 IOSlot 의 PTE 가 valid 한지 검사. simple VA 는 chip PTE 직접 읽고,
			// extended VA 는 chip PTE[6144+L1_idx] (= L2 subtable PA) 가 유효한지
			// + host 의 L2 entry (page PA) 가 유효한지 둘 다 확인.
			{
				ULONG slotIdx;
				for (slotIdx = 0; slotIdx < IO_SLOT_COUNT; slotIdx++) {
					ALLOC_IO_SLOT* slot = &pDc->IOSlots[slotIdx];
					if (slot->Kva == NULL || slot->Size == 0) continue;
					UINT64 va = slot->DeviceVa;
					SIZE_T sz = slot->Size;
					ULONG pageCount = (ULONG)((sz + 0xFFF) >> 12);
					BOOLEAN isExt = (va & (1ULL << 63)) != 0;

					if (!isExt) {
						ULONG pageStart = (ULONG)(va >> 12);
						ULONG invalid = 0;
						UINT64 firstInvalidPte = 0;
						ULONG firstInvalidIdx = 0;
						ULONG pageIdx;
						for (pageIdx = 0; pageIdx < pageCount; pageIdx++) {
							UINT64 pte = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + (pageStart + pageIdx) * 8);
							if ((pte & 1) == 0) {
								if (invalid == 0) {
									firstInvalidPte = pte;
									firstInvalidIdx = pageStart + pageIdx;
								}
								invalid++;
							}
						}
						DbgPrint("[PTE-CHECK] slot[%u] SIMPLE VA=0x%llx size=0x%llx "
							"PTE[%u..%u] (%u pages)  invalid=%u/%u%s\n",
							slotIdx, va, (UINT64)sz,
							pageStart, pageStart + pageCount - 1, pageCount,
							invalid, pageCount,
							(invalid > 0) ? "  *** !!! ***" : "");
						if (invalid > 0) {
							DbgPrint("[PTE-CHECK]   first invalid: PTE[%u] = 0x%llx\n",
								firstInvalidIdx, firstInvalidPte);
						}
					}
					else {
						// Extended VA: walk 2-level PT.
						ULONG invalid = 0;
						ULONG pageIdx;
						if (pDc->ExtPoolKva == NULL) {
							DbgPrint("[PTE-CHECK] slot[%u] EXT VA=0x%llx ExtPool not init — skipped\n",
								slotIdx, va);
							continue;
						}
						for (pageIdx = 0; pageIdx < pageCount; pageIdx++) {
							UINT64 curVa  = va + ((UINT64)pageIdx << PAGE_SHIFT);
							UINT32 l1Idx  = (UINT32)((curVa >> 21) & 0x1FFF);
							UINT32 l2Idx  = (UINT32)((curVa >> 12) & 0x1FF);
							UINT64* l2Tab = (UINT64*)((PUCHAR)pDc->ExtPoolKva +
								((SIZE_T)l1Idx << PAGE_SHIFT));
							UINT64 entry  = l2Tab[l2Idx];
							if ((entry & 1) == 0) invalid++;
						}
						// L1 entry 확인 — head/tail 만.
						{
							UINT32 l1Head = (UINT32)((va >> 21) & 0x1FFF);
							UINT64 endVa  = va + ((UINT64)pageCount << PAGE_SHIFT) - 1;
							UINT32 l1Tail = (UINT32)((endVa >> 21) & 0x1FFF);
							UINT64 l1RegH = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + ((6144u + l1Head) * 8));
							UINT64 l1RegT = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + ((6144u + l1Tail) * 8));
							DbgPrint("[PTE-CHECK] slot[%u] EXT VA=0x%llx size=0x%llx "
								"L1[%u..%u] head=0x%llx tail=0x%llx  L2-invalid=%u/%u%s\n",
								slotIdx, va, (UINT64)sz, l1Head, l1Tail,
								l1RegH, l1RegT, invalid, pageCount,
								(invalid > 0) ? "  *** !!! ***" : "");
						}
					}
				}
			}

			// VA_EXE1_PARAM_DATA / VA_EXE0_PARAM_DATA 부근 PTE 의 PA 가 실제 slot KVA 의 PA 와
			// 일치하는지 spot check (head / mid / tail 3 지점).
			//
			// 두 slot 다 확인:
			//   - IO_SLOT_PARAM_DATA   (6.14MB, VA=0x200000, exe1 caching 용)
			//   - IO_SLOT_EXE0_PARAM   (192KB,  VA=0xA00000, exe0 inference 용)
			// ADD 모델은 exe0_parameters 가 없어서 정상 동작했고 SSD 만 깨지는
			// 증상으로 좁혀졌으므로, exe0 param slot 의 PTE 매핑 무결성이 핵심.
			{
				static const struct {
					IO_SLOT_INDEX idx;
					const char*   tag;
				} kParamSlots[] = {
					{ IO_SLOT_PARAM_DATA, "PARAM(exe1)" },
					{ IO_SLOT_EXE0_PARAM, "PARAM(exe0)" },
				};
				ULONG slotK;
				for (slotK = 0; slotK < sizeof(kParamSlots) / sizeof(kParamSlots[0]); slotK++) {
					ALLOC_IO_SLOT* pSlot = &pDc->IOSlots[kParamSlots[slotK].idx];
					const char*    tag   = kParamSlots[slotK].tag;
					if (pSlot->Kva == NULL || pSlot->Size < 0x1000) {
						DbgPrint("[PTE-PA-CHECK] %s slot empty or too small "
							"(Kva=%p Size=0x%llx) — skipped\n",
							tag, pSlot->Kva, (UINT64)pSlot->Size);
						continue;
					}
					ULONG pageCount = (ULONG)((pSlot->Size + 0xFFF) >> 12);
					BOOLEAN isExt = (pSlot->DeviceVa & (1ULL << 63)) != 0;
					ULONG checkIdx[3] = {
						0,
						pageCount / 2,
						(pageCount > 0) ? (pageCount - 1) : 0
					};
					const char* label[3] = { "HEAD", "MID", "TAIL" };

					if (!isExt) {
						ULONG pageStart = (ULONG)(pSlot->DeviceVa >> 12);
						DbgPrint("[PTE-PA-CHECK] %s SIMPLE slot VA=0x%llx size=0x%llx "
							"PTE[%u..%u] (%u pages)\n",
							tag, pSlot->DeviceVa, (UINT64)pSlot->Size,
							pageStart, pageStart + pageCount - 1, pageCount);
						ULONG checkK;
						for (checkK = 0; checkK < 3; checkK++) {
							ULONG pageIdx = checkIdx[checkK];
							UINT64 pte = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + (pageStart + pageIdx) * 8);
							UINT64 chipPa = pte & ~0xFFFULL;
							PHYSICAL_ADDRESS hostPa =
								MmGetPhysicalAddress((PUCHAR)pSlot->Kva + pageIdx * 0x1000);
							BOOLEAN match = (chipPa == (UINT64)hostPa.QuadPart);
							DbgPrint("[PTE-PA-CHECK] %s %s page[%u] (PTE[%u]): "
								"chipPA=0x%llx hostPA=0x%llx %s\n",
								tag, label[checkK], pageIdx, pageStart + pageIdx,
								chipPa, (UINT64)hostPa.QuadPart,
								match ? "MATCH" : "MISMATCH!!!");
						}
					}
					else {
						// Extended: chip PTE[6144+L1_idx] = L2-subtable PA (host RAM).
						// L2-subtable[L2_idx] = page PA.  Verify both layers.
						if (pDc->ExtPoolKva == NULL) {
							DbgPrint("[PTE-PA-CHECK] %s EXT VA=0x%llx ExtPool not init — skipped\n",
								tag, pSlot->DeviceVa);
							continue;
						}
						DbgPrint("[PTE-PA-CHECK] %s EXT slot VA=0x%llx size=0x%llx (%u pages)\n",
							tag, pSlot->DeviceVa, (UINT64)pSlot->Size, pageCount);
						ULONG checkK;
						for (checkK = 0; checkK < 3; checkK++) {
							ULONG pageIdx = checkIdx[checkK];
							UINT64 curVa  = pSlot->DeviceVa + ((UINT64)pageIdx << PAGE_SHIFT);
							UINT32 l1Idx  = (UINT32)((curVa >> 21) & 0x1FFF);
							UINT32 l2Idx  = (UINT32)((curVa >> 12) & 0x1FF);
							UINT64 chipL1 = apex_read_register(bar2,
								APEX_REG_PAGE_TABLE + ((6144u + l1Idx) * 8));
							UINT64 l1Pa   = chipL1 & ~0xFFFULL;
							UINT64 expectedL1Pa = pDc->ExtPoolPa +
								((UINT64)l1Idx << PAGE_SHIFT);
							UINT64* l2Tab = (UINT64*)((PUCHAR)pDc->ExtPoolKva +
								((SIZE_T)l1Idx << PAGE_SHIFT));
							UINT64 l2Entry = l2Tab[l2Idx];
							UINT64 chipPa  = l2Entry & ~0xFFFULL;
							PHYSICAL_ADDRESS hostPa =
								MmGetPhysicalAddress((PUCHAR)pSlot->Kva + pageIdx * 0x1000);
							BOOLEAN l1Match = (l1Pa == expectedL1Pa);
							BOOLEAN dataMatch = (chipPa == (UINT64)hostPa.QuadPart);
							DbgPrint("[PTE-PA-CHECK] %s %s page[%u] L1[%u]=0x%llx (exp 0x%llx %s) "
								"L2[%u]=0x%llx -> chipPA=0x%llx hostPA=0x%llx %s\n",
								tag, label[checkK], pageIdx, l1Idx, chipL1, expectedL1Pa,
								l1Match ? "OK" : "MISMATCH",
								l2Idx, l2Entry, chipPa, (UINT64)hostPa.QuadPart,
								dataMatch ? "MATCH" : "MISMATCH!!!");
						}
					}
				}
			}

			typedef struct {
				UINT64 address;
				UINT32 size_in_bytes;
				UINT32 reserved;
			} HOST_QUEUE_DESC;
			C_ASSERT(sizeof(HOST_QUEUE_DESC) == 16);

			HOST_QUEUE_DESC* ring = (HOST_QUEUE_DESC*)pDc->DescRingBase;

			ALLOC_IO_SLOT* exe1Slot = &pDc->IOSlots[IO_SLOT_EXE1_BS];
			if (exe1Slot->Kva != NULL && exe1Slot->Size > 0) {
				npudriverDumpPciAer(device, "BeforeSubmit");
				ApexDumpCsrRegions(pDc, "BeforeSubmit");
				UINT32 slot1 = pDc->DescRingTail % 256;
				ring[slot1].address = exe1Slot->DeviceVa;
				ring[slot1].size_in_bytes = (UINT32)exe1Slot->ActualSize;
				ring[slot1].reserved = 0;
				pDc->DescRingTail++;
				DbgPrint("[INFER_NEW] enqueued exe1: VA=0x%llx size=0x%x slot=%u\n",
					exe1Slot->DeviceVa, (UINT32)exe1Slot->Size, slot1);
				KeMemoryBarrier();	// ring write 가 chip 보다 먼저 보이도록
				apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDc->DescRingTail);


				// 임시 코드 
				DbgPrint("[INFER_NEW] enqueued exe1, waiting...\n");
				LARGE_INTEGER t1; t1.QuadPart = -30000000LL;  // 5s
				NTSTATUS s1 = KeWaitForSingleObject(&pDc->InferCompleteEvent,
					Executive, KernelMode, FALSE, &t1);
				if (s1 == STATUS_TIMEOUT) {
					DbgPrint("[INFER_NEW] PARAM_CACHE phase TIMEOUT\n");
					status = STATUS_IO_TIMEOUT;
					break;
				}
				// HIB_ERR check 한 번
				if (apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS) != 0) {
					DbgPrint("[INFER_NEW] PARAM_CACHE phase FAULT\n");
					status = STATUS_DEVICE_HARDWARE_ERROR;
					break;
				}

				// === EXE1-POPCOUNT (2026-05-16) =================================
				// CSR diff vs libedgetpu showed user does only ~3085 PARAMETER_POPs
				// across exe1+exe0 vs libedgetpu's ~30444 (10× difference) — likely
				// only bbox weights cached, score weights never popped.
				// This dump isolates whether exe1 alone is short-popping, OR whether
				// the shortfall is in exe0.
				// Expected libedgetpu-equivalent value at this point: ~30444 (0x76ec)
				// If we see ~3085 (0x0c0d) here → exe1 itself is early-stopping
				// If we see ~30444 here → exe1 OK, problem is exe0 phase
				{
					UINT64 popParam   = apex_read_register(bar2, 0x44058);  // SyncCounter_PARAMETER_POP
					UINT64 popAvdata  = apex_read_register(bar2, 0x44050);  // SyncCounter_AVDATA_POP
					UINT64 infParam   = apex_read_register(bar2, 0x44068);  // SyncCounter_PARAMETER_INFEED
					UINT64 infAvdata  = apex_read_register(bar2, 0x44060);  // SyncCounter_AVDATA_INFEED
					UINT64 infScalar  = apex_read_register(bar2, 0x44070);  // SyncCounter_SCALAR_INFEED
					UINT64 outRing    = apex_read_register(bar2, 0x44088);  // SyncCounter_RING_OUTFEED
					UINT64 curPc      = apex_read_register(bar2, 0x44028);  // currentPc
					UINT64 popStart   = apex_read_register(bar2, 0x441c0);  // parameterPopStartCycle
					UINT64 popEnd     = apex_read_register(bar2, 0x441c8);  // parameterPopEndCycle
					UINT64 popPc      = apex_read_register(bar2, 0x441d0);  // parameterPopProgramCounter
					DbgPrint("[EXE1-POPCOUNT] PARAM_POP=0x%llx (%llu)  PARAM_INFEED=0x%llx  AVDATA_POP=0x%llx  AVDATA_INFEED=0x%llx  SCALAR_INFEED=0x%llx  RING_OUTFEED=0x%llx\n",
						popParam, popParam, infParam, popAvdata, infAvdata, infScalar, outRing);
					DbgPrint("[EXE1-POPCOUNT] currentPc=0x%llx  paramPop_Start=0x%llx  paramPop_End=0x%llx  paramPop_ProgCnt=0x%llx\n",
						curPc, popStart, popEnd, popPc);
				}

				npudriverDumpPciAer(device, "AfterIssueDmas");

				// === [SENTINEL] exe1 캐싱 후 host PARAM 영역 wipe ===
				// 가설 검증: 칩이 PARAMETER_CACHING 으로 weight 를 자기 SRAM 으로 copy 했다면,
				// 여기서 host param 을 망가뜨려도 exe0 결과는 동일해야 함.
				// 반대로 칩이 매 inference 마다 host param 을 다시 read 한다면 output 깨짐.
				//{
				//	ALLOC_IO_SLOT* paramSlot = &pDc->IOSlots[IO_SLOT_PARAM_DATA];
				//	if (paramSlot->Kva != NULL && paramSlot->Size > 0) {
				//		PUCHAR p = (PUCHAR)paramSlot->Kva;
				//		DbgPrint("[SENTINEL] exe1 param BEFORE wipe (first 16B): "
				//			"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				//			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
				//			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

				//		// 0xCC pattern (OUTFEED sentinel 과 동일) — 0x00 보다 눈에 잘 띔
				//		//RtlFillMemory(paramSlot->Kva, paramSlot->Size, 0xCC);

				//		DbgPrint("[SENTINEL] exe1 param AFTER wipe (first 16B): "
				//			"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				//			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
				//			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
				//		DbgPrint("[SENTINEL] exe1 param wiped: %llu bytes filled with 0xCC at KVA=%p\n",
				//			(UINT64)paramSlot->Size, paramSlot->Kva);
				//	}
				//}

				//{
				//	ALLOC_IO_SLOT* exe0ParamSlot = &pDc->IOSlots[IO_SLOT_EXE0_PARAM];
				//	if (exe0ParamSlot->Kva != NULL && exe0ParamSlot->Size > 0) {
				//		RtlFillMemory(exe0ParamSlot->Kva, exe0ParamSlot->Size, 0xDD);
				//		DbgPrint("[SENTINEL] exe0 param wiped (%llu B with 0xDD)\n",
				//			(UINT64)exe0ParamSlot->Size);
				//	}
				//}

				// (계속) exe0 enqueue 코드가 여기 아래

				
			

				//// ★ exe1 끝났으니 다음 phase 위해 reset
				//KeClearEvent(&pDc->InferCompleteEvent);
				//pDc->IsrSeenPendingBits = 0;

				//// exe1 완료 직후, exe0 enqueue 전에 박을 진단:
				//DbgPrint("[POST-EXE1] page_table_size=0x%llx extended=0x%llx translation_en=0x%llx\n",
				//	apex_read_register(bar2, APEX_REG_PAGE_TABLE_SIZE),
				//	apex_read_register(bar2, APEX_REG_EXTENDED_TABLE),
				//	apex_read_register(bar2, 0x46010));

				//// PTE[15] 직접 read (slot 0 input의 fault VA 위치)
				//DbgPrint("[POST-EXE1] PTE[15] = 0x%llx (should be PA|0x1)\n",
				//	apex_read_register(bar2, APEX_REG_PAGE_TABLE + 15 * 8));   // 정확한 register 매크로 확인 필요

				//// HIB_ERROR_MASK 도 확인 (어떤 에러를 활성화하는지 바뀌었는지)
				//DbgPrint("[POST-EXE1] hib_error_mask=0x%llx\n",
				//	apex_read_register(bar2, 0x486f8));

				//arm_tile_and_engiend(bar2, pDc);

				//// input pte 검증 
				//{
				//	DbgPrint("[INFER_NEW] | [CHECK] PTE readback for INPUT range:\n");
				//	size_t input_i;
				//	for (input_i = 0; input_i <= 75; input_i++) {
				//		UINT64 pte = apex_read_register(bar2, APEX_REG_PAGE_TABLE + input_i * 8);
				//		DbgPrint("  PTE[%2u] = 0x%llx  %s\n", input_i, pte, (pte & 1) ? "valid" : "INVALID");
				//	}
				//}
			}

			/*{
				ALLOC_IO_SLOT* exe0ParamSlot = &pDc->IOSlots[IO_SLOT_PARAM_DATA];
				if (exe0ParamSlot->Kva != NULL && exe0ParamSlot->Size > 0) {
					RtlFillMemory(exe0ParamSlot->Kva, exe0ParamSlot->Size, 0xDD);
					DbgPrint("[SENTINEL] exe0 param wiped (%llu B with 0xDD)\n",
						(UINT64)exe0ParamSlot->Size);
				}
			}

			{
				ALLOC_IO_SLOT* exe0ParamSlot = &pDc->IOSlots[IO_SLOT_EXE0_PARAM];
				if (exe0ParamSlot->Kva != NULL && exe0ParamSlot->Size > 0) {
					RtlFillMemory(exe0ParamSlot->Kva, exe0ParamSlot->Size, 0xDD);
					DbgPrint("[SENTINEL] exe0 param wiped (%llu B with 0xDD)\n",
						(UINT64)exe0ParamSlot->Size);
				}
			}*/

			UINT32 slot0 = pDc->DescRingTail % 256;

			ring[slot0].address = pIn->BitstreamDeviceVA;
			ring[slot0].size_in_bytes = (UINT32)pIn->BitstreamSize;
			ring[slot0].reserved = 0;
			pDc->DescRingTail++;
			KeMemoryBarrier();	// ring write 가 chip 보다 먼저 보이도록'

			apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDc->DescRingTail);
			ApexDumpCsrRegions(pDc, "AfterIssueDmas");
		}

		// sc_host_int_count 스냅샷 - 완료 판정 기준선
		pDc->LastScHostIntCount = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);

		// [5] 완료 대기 - IRQ-only (5s timeout = 진짜 실패, 폴링 fallback 없음)
		{
			LARGE_INTEGER t1; t1.QuadPart = -30000000LL;   // 5 s (음수 = relative, 100ns 단위)
			status = KeWaitForSingleObject(&pDc->InferCompleteEvent, Executive, KernelMode, FALSE, &t1);
		}

		if (status == STATUS_TIMEOUT) {
			// 폴링으로 SC_HOST_INT_COUNT 보지 않음 — IRQ가 안 왔다는 사실 자체를 에러로.
			DbgPrint("[INFER_NEW] IRQ TIMEOUT — interrupt path failed\n");
			DbgPrint("[INFER_NEW]   ISR fire count   = %d\n", pDc->IsrCallCount);
			DbgPrint("[INFER_NEW]   IsrSeenPending   = 0x%x\n",
				(UINT32)pDc->IsrSeenPendingBits);
			DbgPrint("[INFER_NEW]   WIRE_INT_PENDING = 0x%llx\n",
				apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING));
			DbgPrint("[INFER_NEW]   SC_HOST_COUNT    = 0x%llx (pre=0x%llx)\n",
				apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT),
				pDc->LastScHostIntCount);
			DbgPrint("[INFER_NEW]   HIB_ERROR        = 0x%llx\n",
				apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS));
			DbgPrint("[INFER_NEW]   SC_RUN/IN/OUT    = 0x%x / 0x%x / 0x%x\n",
				apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS));
			status = STATUS_IO_TIMEOUT;
			break;   // 슬롯 unlock은 IOCTL_FREE_IO_BUFFERS / FileCleanup 책임
		}

		ApexDumpCsrRegions(pDc, "AfterExecution");

		// post-wait 진단
		// dpc는 터미널 상태 도달만 알리고, success / failure 판정은 ioctl 책임
		// anyhalted / fatalSeen 트리거로 깨어났을 수 있으니 에러 레지스터 확인. 
		{
			{
				UINT64 qBase = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_BASE);
				UINT64 qSize = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_SIZE);
				UINT64 qTail = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_TAIL);
				UINT64 qFetch = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_FETCHED_HEAD);
				UINT64 qComplete = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				UINT64 qCtrl = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_CONTROL);
				UINT64 qIntStatus = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS);
				DbgPrint("[INFER_NEW] | CHECK INSTRUCTION | QUEUE_BASE=0x%llx SIZE=0x%llx TAIL=0x%llx FETCHED=0x%llx IQ_FETCHED_HEAD=0x%llx CTRL=0x%llx INT_STATUS=0x%llx (IQ_FETCHED_HEAD is fetch progress, not done)\n",
					qBase, qSize, qTail, qFetch, qComplete, qCtrl, qIntStatus);
				UINT64 qStatusBlockBase = apex_read_register(bar2, 0x48598);
				DbgPrint("[INFER_NEW] STATUS_BLOCK_BASE=0x%llx\n", qStatusBlockBase);
			}

			DbgPrint("[INFER_NEW] | [CHECK] HIB_ERR=0x%llx FIRST_ERR=0x%llx FAULT_VA=0x%llx SC=0x%x INF=0x%x OUT=0x%x PP=0x%x\n",
				apex_read_register(bar2, 0x486f0),
				apex_read_register(bar2, 0x48700),
				apex_read_register(bar2, 0x48738),
				apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS),
				apex_read_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_STATUS));


			UINT32 hibErr = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			UINT32 scErr = apex_read_register_32(bar2, APEX_REG_SCALAR_CORE_ERROR_STATUS);
			UINT32 scStat = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT32 outStat = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
			BOOLEAN fatal = (pDc->IsrSeenPendingBits & APEX_WIRE_BIT_FATAL_ERR) != 0;

			if (fatal || hibErr != 0) {
				// === HIB 진단 — 정확한 레지스터 분리 ===
				//   0x486f0 hib_error_status        : 현재 에러 비트맵 (real-time)   ← 위 hibErr
				//   0x48700 hib_first_error_status  : 첫 에러 비트맵 (latched)
				//   0x48708 hib_first_error_ts     : 첫 에러 cycle 타임스탬프
				//   0x48738 page_fault_address     : ★ 진짜 fault VA latch ★
				//   bit 분해 (common_csr_helper.h:108-109): bit0=inbound_page_fault, bit1=extended_page_fault
				UINT64 hibFirstStatus = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
				UINT64 hibFirstTs     = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR_TS);
				UINT64 faultVA        = apex_read_register(bar2, APEX_REG_INFEED_PAGE_FAULT_ADDR);

				DbgPrint("[INFER_NEW] FATAL: HIB_ERR=0x%x ISR_seen=0x%x SC_ERR=0x%x\n",
					hibErr, (UINT32)pDc->IsrSeenPendingBits, scErr);
				DbgPrint("[INFER_NEW]   hib_error_status(0x486f0)         = 0x%x  (real-time)\n", hibErr);
				DbgPrint("[INFER_NEW]   hib_first_error_status(0x48700)   = 0x%llx  (latched)\n", hibFirstStatus);
				DbgPrint("[INFER_NEW]   hib_first_error_timestamp(0x48708)= 0x%llx\n", hibFirstTs);
				DbgPrint("[INFER_NEW]   page_fault_address(0x48738)       = 0x%llx  ★ fault VA\n", faultVA);

				// 비트 분해 — real-time + latched 합산
				UINT32 combined = hibErr | (UINT32)hibFirstStatus;
				if (combined & (1u<<0))  DbgPrint("[INFER_NEW]   bit0 inbound_page_fault\n");
				if (combined & (1u<<1))  DbgPrint("[INFER_NEW]   bit1 extended_page_fault\n");
				combined &= ~0x3u;
				if (combined)            DbgPrint("[INFER_NEW]   other bits set: 0x%x (별도 디코드 필요)\n", combined);

				// fault VA 가 어느 슬롯/PTE 영역에 해당하는지
				if (faultVA != 0) {
					UINT64 faultPteIdx = faultVA >> 12;
					DbgPrint("[INFER_NEW]   fault VA → PTE[%llu] (%s)\n",
						faultPteIdx,
						(faultPteIdx < 6144) ? "simple" : (faultPteIdx < 8192 ? "extended" : "OUT_OF_RANGE"));

					ULONG s;
					BOOLEAN matchedSlot = FALSE;
					for (s = 0; s < IO_SLOT_COUNT; ++s) {
						ALLOC_IO_SLOT* sl = &pDc->IOSlots[s];
						if (!sl->Kva) continue;
						if (faultVA >= sl->DeviceVa && faultVA < sl->DeviceVa + sl->Size) {
							DbgPrint("[INFER_NEW]   fault VA inside slot %u (DeviceVA=0x%llx size=0x%llx) — 매핑은 됐는데 fault\n",
								s, sl->DeviceVa, (UINT64)sl->Size);
							matchedSlot = TRUE;
							break;
						}
					}
					if (!matchedSlot) {
						DbgPrint("[INFER_NEW]   fault VA가 알려진 슬롯에 없음 — bitstream이 우리가 매핑 안 한 VA를 access함\n");
					}
				} else {
					DbgPrint("[INFER_NEW]   fault VA = 0 → 페이지폴트 아님. instruction stream / descriptor 문제 의심\n");
				}

				status = STATUS_DEVICE_HARDWARE_ERROR;
				break;
			}

			if (scStat == 4 || outStat == 4) {
				DbgPrint("[INFER_NEW] engine halted: SC=0x%x OUT=0x%x SC_ERR=0x%x\n",
					scStat, outStat, scErr);
				status = STATUS_DEVICE_DATA_ERROR;
				break;
			}
		}

		// output kva로 확인하기 
		PUCHAR kva = (PUCHAR)pDc->IOSlots[1].Kva;
		SIZE_T size = pDc->IOSlots[1].Size;
		if (kva != NULL && size > 0) {
			DbgPrint("[DPC-OUT-NEW] kva=%p size=%zu first16: "
				"%02x %02x %02x %02x %02x %02x %02x %02x  "
				"%02x %02x %02x %02x %02x %02x %02x %02x\n",
				kva, size,
				kva[0], kva[1], kva[2], kva[3],
				kva[4], kva[5], kva[6], kva[7],
				kva[8], kva[9], kva[10], kva[11],
				kva[12], kva[13], kva[14], kva[15]);
		}

		// test debug
		UINT64 val_46010 = apex_read_register(bar2, 0x46010);  // translation_enable
		UINT64 val_46000 = apex_read_register(bar2, 0x46000);  // page_table_size
		UINT64 val_48738 = apex_read_register(bar2, 0x48738);  // infeed_page_fault_address
		UINT64 val_486f0 = apex_read_register(bar2, 0x486f0);  // user_hib_error_status

		DbgPrint("[INFER_NEW] [test debug] val_46010=%llu val_46000=%llu val_48738=%llu val_486f0=%llu\n", 
			val_46010, val_46000, val_48738, val_486f0);

		if (pDc->StatusBlockBase != NULL) {
			PUCHAR base = (PUCHAR)pDc->StatusBlockBase;
			DbgPrint("[INFER_NEW] | [test debug] | after desc submit | %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
				base[0], base[1], base[2], base[3], base[4], base[5], base[6], base[7],
				base[8], base[9], base[10], base[11], base[12], base[13], base[14], base[15]);
		}

		// 정상 - dpc 가 (allIdle && scHostSeen) 게이트 통과해서 깨운 경우
		DbgPrint("[INFER_NEW] inference complete via IRQ (ISR fires=%d)\n", pDc->IsrCallCount);
		npudriverDumpPciAer(device, "AfterExecution");
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
				//
				// For extended VAs (bit 63 set in OutputDeviceVA), the LOWER
				// half is often 0 which would match thousands of zero dwords.
				// Skip the LOWER scan in that case and instead scan for the
				// UPPER half (0x80000000) which is unique enough to find.
				{
					BOOLEAN outputIsExtended = (pInput->OutputDeviceVA & (1ULL << 63)) != 0;
					UINT32 patterns[5];
					patterns[0] = (UINT32)pInput->InputDeviceVA;
					patterns[1] = outputIsExtended
						? (UINT32)(pInput->OutputDeviceVA >> 32)               // UPPER (0x80000000)
						: (UINT32)pInput->OutputDeviceVA;                       // LOWER (legacy 0x67c000)
					patterns[2] = outputIsExtended
						? (UINT32)((pInput->OutputDeviceVA + 0x2000) >> 32)
						: (UINT32)(pInput->OutputDeviceVA + 0x2000);
					patterns[3] = 0x3000u;
					patterns[4] = (UINT32)pInput->BitstreamDeviceVA;
					const char *names[5] = {
						"INPUT",
						outputIsExtended ? "OUTPUT[0]_UPPER" : "OUTPUT[0]",
						outputIsExtended ? "OUTPUT[1]_UPPER" : "OUTPUT[1]",
						"PARAM",
						"BITSTREAM_SELF"
					};
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

			if ((pInput->InputDeviceVA & (1ULL << 63)) != 0) {
				// Extended VA: route via 2-level page table (subtable pool).
				NTSTATUS extStatus = ApexExtMapBuffer(device, pInput->InputDeviceVA,
					pfnArray, pageCount);
				if (!NT_SUCCESS(extStatus)) {
					DbgPrint("[%s] INPUT ApexExtMapBuffer failed VA=0x%llx pages=%u status=0x%x\n",
						__FUNCTION__, pInput->InputDeviceVA, pageCount, extStatus);
					status = extStatus;
					MmUnlockPages(inputImageMdl);
					IoFreeMdl(inputImageMdl);
					MmUnlockPages(outputBufferMdl);
					IoFreeMdl(outputBufferMdl);
					break;
				}
				DbgPrint("[%s] EXT registered input: VA=0x%llx pages=%u\n",
					__FUNCTION__, pInput->InputDeviceVA, pageCount);
			} else {
				DbgPrint("[%s] Registering input: PTE[%lu] size=%lu pages\n",
					__FUNCTION__, pteIdx, pageCount);
				WdfSpinLockAcquire(pDevContext->PageTableLock);
				for (i = 0; i < pageCount; i++) {
					UINT64 physAddr = ((UINT64)pfnArray[i] << PAGE_SHIFT);
					apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8), physAddr | 1);
				}
				WdfSpinLockRelease(pDevContext->PageTableLock);

				// == PTE write read back == (simple-VA only)
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

			// ============================================================
			// INPUT-CHK: pre-submit verification dump.
			// 1) Per-page first8 / last8 byte dump for first up-to-4 pages
			//    AND the very last page (so we know the tail of the buffer
			//    isn't garbage).
			// 2) XOR-fold checksum over the entire input region — saved in
			//    DEVICE_CONTEXT for post-DONE comparison.
			// Combined with [POLL@*ms] INFEED=kRun observation and the
			// post-DONE INPUT-VERIFY line, this gives high confidence that
			// the chip's INFEED read the exact host bytes we placed.
			// ============================================================
			{
				PUCHAR kvAddr = (PUCHAR)MmGetSystemAddressForMdlSafe(inputImageMdl, NormalPagePriority);
				if (kvAddr != NULL && pInput->InputImageSize > 0) {
					SIZE_T totalSize = (SIZE_T)pInput->InputImageSize;
					UINT32 dumpFirstN = pageCount < 4 ? pageCount : 4;
					UINT32 pp;

					for (pp = 0; pp < dumpFirstN; pp++) {
						PUCHAR p = kvAddr + pp * PAGE_SIZE;
						SIZE_T base = (SIZE_T)pp * PAGE_SIZE;
						SIZE_T tail = base + PAGE_SIZE - 8;
						if (tail + 8 > totalSize) tail = (totalSize >= 8) ? totalSize - 8 : 0;
						PUCHAR pt = kvAddr + tail;
						DbgPrint("[INPUT-CHK] page[%u] first8=%02x %02x %02x %02x %02x %02x %02x %02x  last8(@0x%llx)=%02x %02x %02x %02x %02x %02x %02x %02x\n",
							pp,
							p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
							(UINT64)tail,
							pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7]);
					}

					// Also dump first8 of LAST page (tail of buffer) so we know
					// the registered PA range covers the whole image.
					if (pageCount > dumpFirstN) {
						UINT32 last = pageCount - 1;
						PUCHAR pl = kvAddr + (SIZE_T)last * PAGE_SIZE;
						DbgPrint("[INPUT-CHK] page[%u] (last) first8=%02x %02x %02x %02x %02x %02x %02x %02x\n",
							last,
							pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], pl[6], pl[7]);
					}

					// XOR-fold: 8-byte stride sum (fast, fits in DPC budget even
					// for 300KB image — ~38K ops). The folded value is invariant
					// to the order of bytes only within each 8-byte qword, but
					// a single byte change anywhere flips the result.
					{
						UINT64 chk = 0;
						SIZE_T qwordCount = totalSize / 8;
						SIZE_T q;
						for (q = 0; q < qwordCount; q++) {
							chk ^= ((UINT64*)kvAddr)[q];
						}
						// Tail bytes (size not multiple of 8): include them as a
						// shifted partial qword so they still affect the sum.
						{
							SIZE_T tailStart = qwordCount * 8;
							UINT64 tailQ = 0;
							SIZE_T t;
							for (t = tailStart; t < totalSize; t++) {
								tailQ |= ((UINT64)kvAddr[t]) << ((t - tailStart) * 8);
							}
							chk ^= tailQ;
						}
						pDevContext->InputChecksumPreSubmit = chk;
						pDevContext->InputChecksumByteCount = (UINT64)totalSize;
						DbgPrint("[INPUT-CHK] xor-fold pre-submit = 0x%016llX over %llu bytes (%lu pages)\n",
							chk, (UINT64)totalSize, pageCount);
					}
				} else {
					pDevContext->InputChecksumPreSubmit = 0;
					pDevContext->InputChecksumByteCount = 0;
					DbgPrint("[INPUT-CHK] WARNING: cannot map input MDL — skipping pre-submit checksum\n");
				}
			}
		}

		// 5. Register output buffer pages in page table.
		//
		// Two paths depending on the OutputDeviceVA bit 63:
		//   (a) bit 63 = 0 → simple region.  Each output page goes into a
		//                    chip PTE register at index (VA >> 12) directly.
		//   (b) bit 63 = 1 → extended region (libedgetpu default).  Build a
		//                    2-level PT: chip PTE register at extended index
		//                    points to a host-resident 4 KB sub-table whose
		//                    entries point at the actual output pages.
		//
		// EXTENDED-PATH BOUNCE: user-mode OUTPUT pages are usually > 4 GB and
		// chip outbound writes truncate to 32-bit, causing HIB_ERR=0x1 +
		// FATAL_ERR.  Workaround: allocate a < 4 GB contiguous kernel bounce
		// buffer, map THAT into the chip's 2-level PT, and memcpy bounce ->
		// user buffer in DPC after OUTFEED drains.
		{
			pageCount = (UINT32)((pInput->OutputBufferSize + PAGE_SIZE - 1) >> PAGE_SHIFT);

			if ((pInput->OutputDeviceVA & (1ULL << 63)) != 0) {
				// Extended VA path with bounce.
				PFN_NUMBER bouncePfns[64];  // 64 pages = 256 KB max bounce
				UINT32 bouncePageCnt = 0;
				NTSTATUS bnStatus = ApexAllocOutputBounce(
					device, (SIZE_T)pInput->OutputBufferSize,
					bouncePfns, ARRAYSIZE(bouncePfns), &bouncePageCnt);
				if (!NT_SUCCESS(bnStatus)) {
					DbgPrint("[%s] ApexAllocOutputBounce failed: 0x%x\n",
						__FUNCTION__, bnStatus);
					status = bnStatus;
					MmUnlockPages(inputImageMdl);
					IoFreeMdl(inputImageMdl);
					MmUnlockPages(outputBufferMdl);
					IoFreeMdl(outputBufferMdl);
					break;
				}
				NTSTATUS extStatus = ApexExtMapBuffer(
					device, pInput->OutputDeviceVA, bouncePfns, bouncePageCnt);
				if (!NT_SUCCESS(extStatus)) {
					DbgPrint("[%s] ApexExtMapBuffer failed: 0x%x — aborting INFER\n",
						__FUNCTION__, extStatus);
					ApexFreeOutputBounce(device);
					status = extStatus;
					MmUnlockPages(inputImageMdl);
					IoFreeMdl(inputImageMdl);
					MmUnlockPages(outputBufferMdl);
					IoFreeMdl(outputBufferMdl);
					break;
				}
				DbgPrint("[%s] Output mapped via EXTENDED+BOUNCE path (VA=0x%llx, %lu pages)\n",
					__FUNCTION__, pInput->OutputDeviceVA, bouncePageCnt);
			} else {
				pfnArray = MmGetMdlPfnArray(outputBufferMdl);
				pteIdx = (UINT32)(pInput->OutputDeviceVA >> PAGE_SHIFT);
				DbgPrint("[%s] Registering output (SIMPLE): PTE[%lu] size=%lu pages\n",
					__FUNCTION__, pteIdx, pageCount);

				WdfSpinLockAcquire(pDevContext->PageTableLock);
				for (i = 0; i < pageCount; i++) {
					UINT64 physAddr = ((UINT64)pfnArray[i] << PAGE_SHIFT);
					// Restore the |0x1 valid bit.  Earlier experiment dropped it
					// to test whether LSB is a read-allowed flag; result was no
					// change, so we put the valid bit back per spec.
					apex_write_register(bar2,
						APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8),
						physAddr | 0x1);
				}
				WdfSpinLockRelease(pDevContext->PageTableLock);
			}

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
			apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL,             1);
			apex_write_register(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL,         1);
			apex_write_register(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL,      1);
			apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL,             1);
			apex_write_register(bar2, APEX_REG_OUTFEED_RUN_CONTROL,            1);
			apex_write_register(bar2, APEX_REG_TILE_OP_RUN_CONTROL,            1);
			apex_write_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL,     1);
			apex_write_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL,     1);
			apex_write_register(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL,          1);
			apex_write_register(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL,          1);
			apex_write_register(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL,          1);
			apex_write_register(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL,          1);
			apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);
			apex_write_register(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);
			apex_write_register(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL,  1);

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

		// MINI CSR SWEEP — dump all named registers right before kicking
		// inference; we dump again at post-DONE.  User diffs by eye / grep
		// to find which registers chip touched during inference.
		npudriverDumpKnownCsrs(pDevContext, "pre-submit");

		// Snapshot SC_HOST_INT_COUNT (BAR2+0x486d0) BEFORE submit. The chip
		// increments this register each time SCALAR executes its host_interrupt 0
		// opcode (placed by the compiler AFTER the OUTFEED drain barrier), so
		// any post-submit increment unambiguously means OUTFEED has finished.
		// We cache the pre-submit value and compare against subsequent reads
		// instead of relying on IQ_COMPLETED_HEAD (which only reflects ring
		// fetch progress, not pipeline completion).
		{
			UINT64 preCount = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);
			pDevContext->LastScHostIntCount = preCount;
			DbgPrint("[INFER] pre-submit SC_HOST_INT_COUNT snapshot = 0x%llx\n", preCount);
		}

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

			// SENTINEL FILL: pre-fill DescRing remainder + StatusBlock with
			// 0xA5 so we can tell after inference if chip stray-wrote into
			// these regions.  Our 2 descriptors will overwrite first 32 bytes.
			RtlFillMemory(pDevContext->DescRingBase, PAGE_SIZE, 0xA5);
			if (pDevContext->StatusBlockBase != NULL) {
				RtlFillMemory(pDevContext->StatusBlockBase, PAGE_SIZE, 0xA5);
			}

			// HIB_OUTPUT_ACTV_CREDITS write removed — working libedgetpu trace
			// never writes this register. Chip POR default of 0 is correct;
			// writes were ignored (readback always 0) anyway. Just log for diag.
			{
				UINT32 outCreditPre = apex_read_register_32(bar2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
				DbgPrint("[INFER] HIB_OUTPUT_ACTV_CREDITS = 0x%x (left at POR, working trace doesn't write)\n",
					outCreditPre);
			}

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

				UINT64 iqFetched = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				DbgPrint("[INFER_WITH_PARAM] PARAM slot=%u VA=0x%llx size=0x%x | INFER slot=%u VA=0x%llx size=0x%x | TAIL=%u IQ_FETCHED=%llu (fetch only)\n",
					slotP, ring[slotP].address, ring[slotP].size_in_bytes,
					slotI, ring[slotI].address, ring[slotI].size_in_bytes,
					pDevContext->DescRingTail, iqFetched);
			} else {
				UINT32 slot = pDevContext->DescRingTail % 256;
				ring[slot].address       = pInput->BitstreamDeviceVA;
				ring[slot].size_in_bytes = (UINT32)pInput->BitstreamSize;
				ring[slot].reserved      = 0;
				KeMemoryBarrier();

				pDevContext->DescRingTail++;
				apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDevContext->DescRingTail);

				UINT64 iqFetched = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
				DbgPrint("[%s] Descriptor submitted: slot=%u VA=0x%llx size=0x%x TAIL=%u IQ_FETCHED=%llu (fetch only — not inference done)\n",
					__FUNCTION__, slot,
					ring[slot].address, ring[slot].size_in_bytes, pDevContext->DescRingTail, iqFetched);

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
			DbgPrint("[DIAG] QUEUE_BASE=0x%llx SIZE=0x%llx TAIL=0x%llx FETCHED=0x%llx IQ_FETCHED_HEAD=0x%llx CTRL=0x%llx INT_STATUS=0x%llx (IQ_FETCHED_HEAD is fetch progress, not done)\n",
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
			UINT32 infeedRunTicks = 0;
			UINT32 outfeedRunTicks = 0;
			for (qi = 0; qi < 20; qi++) {
				LARGE_INTEGER d; d.QuadPart = -10000LL; // 1ms
				KeDelayExecutionThread(KernelMode, FALSE, &d);
				UINT32 inf      = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
				UINT32 out      = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
				UINT32 avd      = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
				UINT32 hibErr   = apex_read_register_32(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
				UINT64 faultVA  = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
				UINT64 infFault = apex_read_register(bar2, APEX_REG_INFEED_PAGE_FAULT_ADDR);
				if (inf == 1) infeedRunTicks++;
				if (out == 1) outfeedRunTicks++;
				DbgPrint("[POLL@%dms] INFEED=0x%x OUTFEED=0x%x AVDATA=0x%x | HIB_ERR=0x%x FAULT_VA=0x%llx INFEED_FAULT=0x%llx\n",
					qi + 1, inf, out, avd, hibErr, faultVA, infFault);
				if (hibErr != 0 || out != 0) break;
			}
			// kRun tick summary — INFEED kRun for >=1ms is strong evidence the
			// engine actually pulled input from host RAM; otherwise it never ran.
			DbgPrint("[INPUT-CHK] INFEED kRun ticks=%u  OUTFEED kRun ticks=%u  (each tick ≈1ms)\n",
				infeedRunTicks, outfeedRunTicks);
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
			// Stage 1 expired — check inference completion via SC_HOST_INT_COUNT.
			//
			// PRIMARY signal: SC_HOST_INT_COUNT (0x486d0) increments above the
			// pre-submit snapshot. This is the SCALAR host_interrupt 0 counter
			// — fires AFTER the OUTFEED drain barrier, so an increment proves
			// the inference pipeline (INFEED → compute → OUTFEED) is fully
			// drained and host RAM has the results.
			//
			// SECONDARY corroboration: SCALAR && OUTFEED in terminal state
			// (kIdle=0 or kHalted=4). Mirrors libedgetpu IsAllRunStatusKIdle.
			//
			// Note: IQ_COMPLETED_HEAD is logged for diagnostics ONLY — it
			// reflects ring descriptor fetch, not pipeline completion.
			UINT64 preCount      = pDevContext->LastScHostIntCount;
			UINT64 scHost50ms    = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);
			UINT32 scStatus50ms  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
			UINT32 outStatus50ms = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
			UINT64 iqFetch50ms   = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
			UINT64 hibErr50ms    = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[50MS] SC=0x%x OUT=0x%x SC_HOST_COUNT=0x%llx (pre=0x%llx) IQ_FETCH=0x%llx TAIL=%u HIB_ERROR=0x%llx\n",
				scStatus50ms, outStatus50ms, scHost50ms, preCount, iqFetch50ms,
				pDevContext->DescRingTail, hibErr50ms);

			// Fast path: SC_HOST count incremented AND SCALAR/OUTFEED terminal
			if (scHost50ms > preCount &&
			    (scStatus50ms == 0 || scStatus50ms == 4) &&
			    (outStatus50ms == 0 || outStatus50ms == 4)) {
				DbgPrint("[50MS] inference truly done (SC_HOST_COUNT 0x%llx>0x%llx SC=0x%x OUT=0x%x)\n",
					scHost50ms, preCount, scStatus50ms, outStatus50ms);
				// Force the InferCompleteEvent so a deferred DPC doesn't
				// leave us hanging — DPC may have observed SC_HOST_0 only
				// and bailed out without unlocking, so we take over here.
				KeSetEvent(&pDevContext->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
				status = STATUS_SUCCESS;
			}

			// Stage 2: poll until SC_HOST count > pre AND SCALAR/OUTFEED terminal (max 5s)
			if (status == STATUS_TIMEOUT) {
				int pollIdx;
				for (pollIdx = 0; pollIdx < 1000; pollIdx++) {
					UINT64 scHost = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);
					UINT32 sc     = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
					UINT32 out    = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
					if (scHost > preCount &&
					    (sc == 0 || sc == 4) &&
					    (out == 0 || out == 4)) {
						UINT64 iqFetchDone = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
						DbgPrint("[POLL] inference done after %d ms (SC_HOST_COUNT=0x%llx>0x%llx SC=0x%x OUT=0x%x IQ_FETCH=0x%llx TAIL=%u)\n",
							50 + pollIdx, scHost, preCount, sc, out, iqFetchDone, pDevContext->DescRingTail);
						KeSetEvent(&pDevContext->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
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
						UINT64 iqFetchSnap= apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
						DbgPrint("[SNAP@%dms] SC=0x%x INFEED=0x%x OUTFEED=0x%x WIRE=0x%llx SC_HOST=0x%llx(pre=0x%llx) IQ_FETCH=0x%llx HIB=0x%llx\n",
							50 + pollIdx, scSnap, infeedSnap, outfedSnap, wirePend,
							scHost, preCount, iqFetchSnap, hibSnap);
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

				// MINI CSR SWEEP — dump the same named registers post-DONE.
				// User diffs the [CSR@pre-submit] vs [CSR@post-DONE] lines.
				npudriverDumpKnownCsrs(pDevContext, "post-DONE");

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

				// ARBITER counters — pinpoints WHERE chip lost OUTFEED data.
				//
				// write_request_arbiter: AXI write channel selection.  Compare
				// output_actv (OUTFEED) vs status_block (known-working) to see
				// whether OUTFEED ever requested the AXI write channel.
				//
				// address_translation_arbiter: MMU walker.  request==0 means
				// the OUTFEED request never even reached MMU (chip-internal
				// path stopped earlier — likely SCALAR/OUTFEED engine never
				// emitted the write).  request>0 + blocked dominant means
				// MMU lookup failed (PTE walk problem).
				{
					UINT32 wraOutReq    = apex_read_register_32(bar2, APEX_REG_WRA_OUT_ACTV_REQ);
					UINT32 wraOutBlk    = apex_read_register_32(bar2, APEX_REG_WRA_OUT_ACTV_BLOCKED);
					UINT32 wraSbReq     = apex_read_register_32(bar2, APEX_REG_WRA_STATUS_BLK_REQ);
					UINT32 wraSbBlk     = apex_read_register_32(bar2, APEX_REG_WRA_STATUS_BLK_BLOCKED);
					DbgPrint("[ARB-W@post-DONE]  out_actv:    req=0x%x blocked=0x%x | status_blk: req=0x%x blocked=0x%x\n",
						wraOutReq, wraOutBlk, wraSbReq, wraSbBlk);

					UINT32 ataOutReq    = apex_read_register_32(bar2, APEX_REG_ATA_OUT_ACTV_REQ);
					UINT32 ataOutBlk    = apex_read_register_32(bar2, APEX_REG_ATA_OUT_ACTV_BLOCKED);
					UINT32 ataInstrReq  = apex_read_register_32(bar2, APEX_REG_ATA_INSTRUCTION_REQ);
					UINT32 ataInstrBlk  = apex_read_register_32(bar2, APEX_REG_ATA_INSTRUCTION_BLOCKED);
					UINT32 ataInActvReq = apex_read_register_32(bar2, APEX_REG_ATA_INPUT_ACTV_REQ);
					UINT32 ataInActvBlk = apex_read_register_32(bar2, APEX_REG_ATA_INPUT_ACTV_BLOCKED);
					DbgPrint("[ARB-AT@post-DONE] out_actv:    req=0x%x blocked=0x%x\n",
						ataOutReq, ataOutBlk);
					DbgPrint("[ARB-AT@post-DONE] instruction: req=0x%x blocked=0x%x\n",
						ataInstrReq, ataInstrBlk);
					DbgPrint("[ARB-AT@post-DONE] input_actv:  req=0x%x blocked=0x%x  (baseline — INFEED works)\n",
						ataInActvReq, ataInActvBlk);

					UINT32 rArb  = apex_read_register_32(bar2, APEX_REG_READ_REQUEST_ARBITER);
					UINT32 wArb  = apex_read_register_32(bar2, APEX_REG_WRITE_REQUEST_ARBITER);
					UINT32 atArb = apex_read_register_32(bar2, APEX_REG_ADDR_TRANSLATION_ARBITER);
					DbgPrint("[ARB-CFG@post-DONE] read_req=0x%x write_req=0x%x addr_trans=0x%x\n",
						rArb, wArb, atArb);
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

				// 1) DescRing — 4 KB pre-filled with 0xA5 before INFER.  Our 2
				//    descriptors occupied first 32 bytes.  Anything past that
				//    still 0xA5 == chip didn't touch.  Non-A5 bytes past 32
				//    == chip stray-wrote here (unexpected target).
				if (pDevContext->DescRingBase != NULL) {
					PUCHAR p = (PUCHAR)pDevContext->DescRingBase;
					ULONG nonA5 = 0;
					ULONG firstNonA5 = 0xFFFFFFFF;
					ULONG ii;
					for (ii = 32; ii < PAGE_SIZE; ii++) {
						if (p[ii] != 0xA5) {
							nonA5++;
							if (firstNonA5 == 0xFFFFFFFF) firstNonA5 = ii;
						}
					}
					DbgPrint("[SCAN-DescRing] PA=0x%llx descs[0..31]=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | post-32 non-A5 bytes=%lu firstAt=0x%lx\n",
						pDevContext->DescRingDeviceVA,
						p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
						p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
						p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
						p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31],
						nonA5, firstNonA5);
					if (nonA5 > 0 && firstNonA5 != 0xFFFFFFFF) {
						ULONG d = firstNonA5;
						DbgPrint("[SCAN-DescRing] firstNonA5@0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							d,
							p[d+0],p[d+1],p[d+2],p[d+3], p[d+4],p[d+5],p[d+6],p[d+7],
							p[d+8],p[d+9],p[d+10],p[d+11], p[d+12],p[d+13],p[d+14],p[d+15]);
					}
				}

				// 2) StatusBlock — 4 KB pre-filled with 0xA5 before INFER.
				//    chip writes completed_head at byte [0..3] (we already
				//    see 0x02 there).  Anything else == stray writes.
				if (pDevContext->StatusBlockBase != NULL) {
					PUCHAR p = (PUCHAR)pDevContext->StatusBlockBase;
					ULONG nonA5 = 0;
					ULONG firstNonA5 = 0xFFFFFFFF;
					ULONG ii;
					for (ii = 16; ii < PAGE_SIZE; ii++) {
						if (p[ii] != 0xA5) {
							nonA5++;
							if (firstNonA5 == 0xFFFFFFFF) firstNonA5 = ii;
						}
					}
					DbgPrint("[SCAN-StatusBlock] head[0..31]=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | post-16 non-A5 bytes=%lu firstAt=0x%lx\n",
						p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
						p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15],
						p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
						p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31],
						nonA5, firstNonA5);
					if (nonA5 > 0 && firstNonA5 != 0xFFFFFFFF) {
						ULONG d = firstNonA5;
						DbgPrint("[SCAN-StatusBlock] firstNonA5@0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
							d,
							p[d+0],p[d+1],p[d+2],p[d+3], p[d+4],p[d+5],p[d+6],p[d+7],
							p[d+8],p[d+9],p[d+10],p[d+11], p[d+12],p[d+13],p[d+14],p[d+15]);
					}
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

				// ============================================================
				// INPUT-VERIFY: post-DONE re-read of input image.
				// Recompute XOR-fold checksum of the input region and compare
				// against pre-submit value.
				//   PASS  → chip did not write into input region (host RAM
				//           contents stable).  Combined with INFEED kRun
				//           observation + PTE readback OK earlier, this is
				//           strong indirect evidence chip read OUR bytes.
				//   CHANGED → chip stray-wrote into input region — bug
				//             (PTE points wrong / chip thinks output VA is
				//             input VA / etc).
				// ============================================================
				if (pDevContext->InferInputMdl != NULL && pDevContext->InputChecksumByteCount > 0) {
					PUCHAR ikva = (PUCHAR)MmGetSystemAddressForMdlSafe(
						pDevContext->InferInputMdl, NormalPagePriority);
					if (ikva != NULL) {
						SIZE_T totalSize = (SIZE_T)pDevContext->InputChecksumByteCount;
						UINT64 chkPre = pDevContext->InputChecksumPreSubmit;

						// Same XOR-fold algorithm as pre-submit
						UINT64 chk = 0;
						SIZE_T qwordCount = totalSize / 8;
						SIZE_T q;
						for (q = 0; q < qwordCount; q++) {
							chk ^= ((UINT64*)ikva)[q];
						}
						{
							SIZE_T tailStart = qwordCount * 8;
							UINT64 tailQ = 0;
							SIZE_T t;
							for (t = tailStart; t < totalSize; t++) {
								tailQ |= ((UINT64)ikva[t]) << ((t - tailStart) * 8);
							}
							chk ^= tailQ;
						}

						DbgPrint("[INPUT-VERIFY] xor-fold post-DONE = 0x%016llX (pre=0x%016llX) → %s\n",
							chk, chkPre,
							(chk == chkPre) ? "PASS (host RAM stable)" : "CHANGED (chip stray-wrote!)");

						// Also dump first 8 bytes of page[0] for visual sanity
						DbgPrint("[INPUT-VERIFY] page[0] first8 post-DONE=%02x %02x %02x %02x %02x %02x %02x %02x\n",
							ikva[0], ikva[1], ikva[2], ikva[3],
							ikva[4], ikva[5], ikva[6], ikva[7]);
					} else {
						DbgPrint("[INPUT-VERIFY] WARNING: cannot map input MDL post-DONE\n");
					}
				}

				// Output buffer kernel-side dump BEFORE unlock — catches the case
				// where chip claims SC==kIdle but OUTFEED never actually wrote.
				if (pDevContext->InferOutputMdl != NULL) {
					PMDL outMdl    = pDevContext->InferOutputMdl;
					UINT64 outVA   = pDevContext->InferOutputDeviceVA;
					UINT64 outSize = pDevContext->InferOutputSize;
					UINT32 outPageCnt = (UINT32)((outSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
					PPFN_NUMBER outPfn = MmGetMdlPfnArray(outMdl);
					BOOLEAN outIsExtended = (outVA & (1ULL << 63)) != 0;

					// Verify PTE readback. Simple = chip PTE[VA>>12] holds data PA.
					// Extended = chip PTE[6144+...] holds 2-level PT PA; sub-entries
					// hold data PAs and live in host RAM.
					if (!outIsExtended) {
						UINT32 outPteIdx = (UINT32)(outVA >> PAGE_SHIFT);
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
					} else {
						UINT32 subtableIdx    = (UINT32)((outVA >> 21) & 0x1FFF);
						UINT32 chipPteIdx     = 6144u + subtableIdx;
						UINT32 hostTableStart = (UINT32)((outVA >> 12) & 0x1FF);
						UINT64 chipReg = apex_read_register(bar2,
							APEX_REG_PAGE_TABLE + (chipPteIdx * 8));
						UINT64 expectPa = pDevContext->ExtPoolPa +
							((UINT64)subtableIdx << PAGE_SHIFT);
						DbgPrint("[DONE-PTE] EXT chip PTE[%u] = 0x%llx (expected pool sub-region PA = 0x%llx | 0x1) %s\n",
							chipPteIdx, chipReg, expectPa,
							((chipReg & ~1ULL) == expectPa) ? "OK" : "MISMATCH");
						if (pDevContext->ExtPoolKva != NULL) {
							UINT64* slot = (UINT64*)((PUCHAR)pDevContext->ExtPoolKva +
								((SIZE_T)subtableIdx << PAGE_SHIFT));
							UINT32 vc = (outPageCnt < 4) ? outPageCnt : 4;
							UINT32 ii;
							for (ii = 0; ii < vc; ii++) {
								UINT64 expectPA = (UINT64)outPfn[ii] << PAGE_SHIFT;
								UINT64 entry = slot[hostTableStart + ii];
								UINT64 entryPa = entry & ~1ULL;
								DbgPrint("[DONE-PTE] EXT 2L[%u+%u] expect=0x%llx entry=0x%llx %s\n",
									hostTableStart, ii, expectPA, entry,
									(entryPa == expectPA) ? "OK" : "MISMATCH");
							}
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
			UINT64 scHostCount   = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);
			UINT64 scHostStatus  = apex_read_register(bar2, APEX_REG_SC_HOST_INT_STATUS);
			UINT64 scHostPre     = pDevContext->LastScHostIntCount;

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
			DbgPrint("[TIMEOUT] IQ_FETCHED_HEAD     = 0x%llx (descriptor fetch progress, NOT INFER done)\n", qCompleted);
			DbgPrint("[TIMEOUT] IQ_INT_STATUS      = 0x%llx\n", qIntStatus);
			DbgPrint("[TIMEOUT] IQ_CONTROL         = 0x%llx\n", qCtrl);
			DbgPrint("[TIMEOUT] SC_HOST_INT_COUNT  = 0x%llx (pre-submit=0x%llx) %s\n",
				scHostCount, scHostPre,
				(scHostCount > scHostPre) ? "*** INCREMENTED — INFER actually finished but DPC missed it" : "no increment — INFER never completed");
			DbgPrint("[TIMEOUT] SC_HOST_INT_STATUS = 0x%llx\n", scHostStatus);

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

		// Tear down ONLY the INFER-scoped mappings (INPUT + OUTPUT).
		// In the bulk-pool design, ApexPageTableUnmap zeroes only the per-page
		// entries inside the pre-allocated 8 MB pool — chip PTE registers and
		// pool itself stay intact, so cached PARAM data / PARAM bitstream
		// mappings are unaffected.
		if (pDevContext->InferInputDeviceVA != 0 && pDevContext->InferInputSize != 0) {
			ApexPageTableUnmap(device,
				pDevContext->InferInputDeviceVA,
				(SIZE_T)pDevContext->InferInputSize);
		}
		if (pDevContext->InferOutputDeviceVA != 0 && pDevContext->InferOutputSize != 0) {
			ApexPageTableUnmap(device,
				pDevContext->InferOutputDeviceVA,
				(SIZE_T)pDevContext->InferOutputSize);
		}

		// Free the < 4 GB output bounce buffer.  DPC already memcpy'd
		// bounce -> user buffer before unlocking the output MDL, so no
		// data is lost here.  Idempotent.
		ApexFreeOutputBounce(device);

		// POST-INFER PCIe snapshot — at PASSIVE_LEVEL (IOCTL caller thread)
		// so config-space reads are safe.  Critical comparison vs the
		// [PCIE@pre-submit] / [PCI-CMD@pre-submit] / [AXI@pre-submit] lines
		// captured before submit:
		//   - aw_ins/w_ins delta = 0  → chip never issued outbound write TLPs
		//                                (chip-internal ATU / credit gate)
		//   - aw_ins/w_ins delta > 0  → chip issued TLPs but they got blocked
		//                                outside the chip; check Status RcvMA
		//                                and DevStatus UnsupReq for evidence.
		if (bar2 != NULL) {
			UINT32 awIns = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION);
			UINT32 wIns  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_INSERTION);
			UINT32 awOcc = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY);
			UINT32 wOcc  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY);
			DbgPrint("[AXI@post-infer] aw_ins=0x%x w_ins=0x%x aw_occ=0x%x w_occ=0x%x\n",
				awIns, wIns, awOcc, wOcc);
		}
		npudriverDumpPciCommand(device, "post-infer");
		npudriverDumpPciAer(device, "post-infer");

		break;
	}
	case IOCTL_PARAM_CACHE_NEW: 
	{

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

		// 0. Release previously cached PARAM data (re-cache).
		// MUST clear chip PTE / 2-level PT entries before unlocking the MDL —
		// otherwise the chip walks stale PFNs into pages the OS has already
		// reclaimed, corrupting kernel memory on the next inference (BSOD).
		// ApexPageTableUnmap dispatches on bit63 to handle extended/simple.
		if (pDevContext->CachedParamMdl != NULL) {
			SIZE_T sz = (SIZE_T)pDevContext->CachedParamPageCount << PAGE_SHIFT;
			DbgPrint("[PARAM_CACHE] Releasing previously cached params: VA=0x%llx (%u pages)\n",
				pDevContext->CachedParamDeviceVA, pDevContext->CachedParamPageCount);
			ApexPageTableUnmap(device, pDevContext->CachedParamDeviceVA, sz);
			MmUnlockPages(pDevContext->CachedParamMdl);
			IoFreeMdl(pDevContext->CachedParamMdl);
			pDevContext->CachedParamMdl = NULL;
			pDevContext->CachedParamDeviceVA = 0;
			pDevContext->CachedParamPteIdx = 0;
			pDevContext->CachedParamPageCount = 0;
		}

		// 0b. Release previously cached PARAM bitstream too — same reason.
		if (pDevContext->CachedParamBitstreamMdl != NULL) {
			SIZE_T sz = (SIZE_T)pDevContext->CachedParamBitstreamPageCount << PAGE_SHIFT;
			DbgPrint("[PARAM_CACHE] Releasing previously cached bitstream: VA=0x%llx (%u pages)\n",
				pDevContext->CachedParamBitstreamDeviceVA,
				pDevContext->CachedParamBitstreamPageCount);
			ApexPageTableUnmap(device, pDevContext->CachedParamBitstreamDeviceVA, sz);
			MmUnlockPages(pDevContext->CachedParamBitstreamMdl);
			IoFreeMdl(pDevContext->CachedParamBitstreamMdl);
			pDevContext->CachedParamBitstreamMdl = NULL;
			pDevContext->CachedParamBitstreamDeviceVA = 0;
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

		// 2. Register parameter pages in page table at ParamDeviceVA.
		// Extended VA path: route through ApexExtMapBuffer (multi-subtable pool).
		pteIdx    = (UINT32)(pInput->ParamDeviceVA >> PAGE_SHIFT);
		pageCount = (UINT32)((pInput->ParamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
		pfnArray  = MmGetMdlPfnArray(paramMdl);

		if ((pInput->ParamDeviceVA & (1ULL << 63)) != 0) {
			NTSTATUS extStatus = ApexExtMapBuffer(device, pInput->ParamDeviceVA,
				pfnArray, pageCount);
			if (!NT_SUCCESS(extStatus)) {
				DbgPrint("[PARAM_CACHE] ApexExtMapBuffer failed VA=0x%llx pages=%u status=0x%x\n",
					pInput->ParamDeviceVA, pageCount, extStatus);
				MmUnlockPages(paramMdl);
				IoFreeMdl(paramMdl);
				status = extStatus;
				break;
			}
			DbgPrint("[PARAM_CACHE] EXT registered param: VA=0x%llx pages=%u\n",
				pInput->ParamDeviceVA, pageCount);
		} else {
			DbgPrint("[PARAM_CACHE] Registering param PTEs: PTE[%u..%u] (%u pages)\n",
				pteIdx, pteIdx + pageCount - 1, pageCount);
			WdfSpinLockAcquire(pDevContext->PageTableLock);
			for (i = 0; i < pageCount; i++)
				apex_write_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + i) * 8),
					((UINT64)pfnArray[i] << PAGE_SHIFT) | 1);
			WdfSpinLockRelease(pDevContext->PageTableLock);
		}

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
			pDevContext->CachedParamMdl        = paramMdl;
			pDevContext->CachedParamDeviceVA   = pInput->ParamDeviceVA;
			pDevContext->CachedParamPteIdx     = pteIdx;
			pDevContext->CachedParamPageCount  = pageCount;
			DbgPrint("[PARAM_CACHE] Cached params retained: VA=0x%llx PTE[%u..%u] (%u pages, MDL=%p)\n",
				pInput->ParamDeviceVA, pteIdx, pteIdx + pageCount - 1, pageCount, paramMdl);

			// Transfer PARAM bitstream MDL ownership from LockedModelMdl to
			// CachedParamBitstreamMdl. The PARAM bitstream's device VA was
			// stored in the most recent IOCTL_MAP_BUFFER call — preserve it
			// (extended VA in current build) instead of hard-coding 0.
			if (pDevContext->LockedModelMdl != NULL && pInput->BitstreamSize > 0) {
				UINT32 bsPageCount = (UINT32)((pInput->BitstreamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
				pDevContext->CachedParamBitstreamMdl       = pDevContext->LockedModelMdl;
				pDevContext->CachedParamBitstreamDeviceVA  = pDevContext->LastMapBufferDeviceVA;
				pDevContext->CachedParamBitstreamSize      = (UINT32)pInput->BitstreamSize;
				pDevContext->CachedParamBitstreamPteIdx    = (UINT32)(pDevContext->LastMapBufferDeviceVA >> PAGE_SHIFT);
				pDevContext->CachedParamBitstreamPageCount = bsPageCount;
				pDevContext->LockedModelMdl  = NULL;
				pDevContext->LockedModelSize = 0;
				DbgPrint("[PARAM_CACHE] Cached bitstream retained: DeviceVA=0x%llx size=0x%x MDL=%p\n",
					pDevContext->CachedParamBitstreamDeviceVA,
					(UINT32)pInput->BitstreamSize,
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
	case IOCTL_ALLOC_IO_BUFFERS:
	{
		DbgPrint("IOCTL_ALLOC_IO_BUFFERS Start!\n");
		npudriverDumpPciAer(device, "AfterOpen");
		PDEVICE_CONTEXT pDC = DeviceGetContext(device);
		WDFMEMORY inMem, outMem;
		IOCTL_ALLOC_IO_BUFFERS_IN* pIn = NULL;
		IOCTL_ALLOC_IO_BUFFERS_OUT* pOut = NULL;
		int i;

		if (InputBufferLength < sizeof(*pIn) ||
			OutputBufferLength < sizeof(*pOut)
			) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		status = WdfRequestRetrieveInputMemory(Request, &inMem);
		if (!NT_SUCCESS(status)) break;
		status = WdfRequestRetrieveOutputMemory(Request, &outMem);
		if (!NT_SUCCESS(status)) break;

		pIn = (IOCTL_ALLOC_IO_BUFFERS_IN*)WdfMemoryGetBuffer(inMem, NULL);
		pOut = (IOCTL_ALLOC_IO_BUFFERS_OUT*)WdfMemoryGetBuffer(outMem, NULL);
		IOCTL_ALLOC_IO_BUFFERS_IN in = *pIn;
		RtlZeroMemory(pOut, sizeof(*pOut));

		struct { UINT64 size, devVa; UINT64* outUserVa, * outPa; } req[IO_SLOT_COUNT] = {
			{ in.InputSize,         in.InputDeviceVA,         &pOut->InputUserVA,         &pOut->InputPa },
			{ in.OutputSize,        in.OutputDeviceVA,        &pOut->OutputUserVA,        &pOut->OutputPa },
			{ in.ScratchSize,       in.ScratchDeviceVA,       &pOut->ScratchUserVA,       &pOut->ScratchPa },
			{ in.Exe0BitstreamSize, in.Exe0BitstreamDeviceVA, &pOut->Exe0BitStreamUserVA, &pOut->Exe0BitstreamPa },
			{ in.ParamDataSize,     in.ParamDataDeviceVA,     &pOut->ParamDataUserVA,     &pOut->ParamDataPa },
			{ in.Exe1BitstreamSize, in.Exe1BitstreamDeviceVA, &pOut->Exe1BitstreamUserVA, &pOut->Exe1BitstreamPa },
			{ in.Exe0ParamSize,		in.Exe0ParamDeviceVA	, &pOut->Exe0ParamUserVA	, &pOut->Exe0ParamPa},
		};

		// 한 번에 셋 다 잡고 셋 다 매핑한다. 중간에 실패하면 이미 잡힌거 전부 되돌림
		for (i = 0; i < IO_SLOT_COUNT; i++) {
			ALLOC_IO_SLOT* slot = &pDC->IOSlots[i];
			SIZE_T size4k;
			PHYSICAL_ADDRESS lo, hi, none;
			PHYSICAL_ADDRESS pa;

			if (req[i].size == 0) continue;
			if (slot->Kva != NULL) {
				// 이미 잡혀있다면 명시적 free 강제. (재진입 방지)
				DbgPrint("[ALLOC_IO] slot %d already allocated — call IOCTL_FREE_IO_BUFFERS first\n", i);
				status = STATUS_DEVICE_BUSY;
				goto alloc_io_fail;
			}

			size4k = (SIZE_T)((req[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
			lo.QuadPart = 0;
			hi.QuadPart = 0xFFFFFFFFLL; // < 4GB. chip MMU 가 32-bit PA만 받으면 필수.
			none.QuadPart = 0;

			slot->Kva = MmAllocateContiguousMemorySpecifyCache(size4k, lo, hi, none, MmNonCached);
			if (slot->Kva == NULL) {
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto alloc_io_fail;
			}
			RtlZeroMemory(slot->Kva, size4k);
			slot->Size = size4k;
			slot->DeviceVa = req[i].devVa;
			slot->ActualSize = req[i].size;

			pa = MmGetPhysicalAddress(slot->Kva);

			// user-mode 매핑 - calling process 컨텍스트에서만 호출되면 안전
			slot->Mdl = IoAllocateMdl(slot->Kva, (ULONG)size4k, FALSE, FALSE, NULL);
			if (slot->Mdl == NULL) {
				MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto alloc_io_fail;
			}
			MmBuildMdlForNonPagedPool(slot->Mdl); // contiguous-nonpaged 라 ok

			__try {
				slot->UserVa = MmMapLockedPagesSpecifyCache(
					slot->Mdl,
					UserMode,
					MmCached,
					NULL,
					FALSE,
					NormalPagePriority
				);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				slot->UserVa = NULL;
			}

			if (slot->UserVa == NULL) {
				IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
				MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto alloc_io_fail;
			}

			{
				UINT64 baseDevVa = slot->DeviceVa;
				UINT64 basePa = (UINT64)pa.QuadPart;
				UINT32 pageCount = (UINT32)(size4k >> PAGE_SHIFT);
				BOOLEAN isExtended = (baseDevVa & 0x8000000000000000ULL) != 0;
				UINT32 j;

				// 4kb check
				if ((baseDevVa & (PAGE_SIZE - 1)) != 0) {
					DbgPrint("[ALLOC_IO] slot %d DeviceVa=0x%llx not page-aligned\n", i, baseDevVa);
					MmUnmapLockedPages(slot->UserVa, slot->Mdl);
					IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
					MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
					slot->UserVa = NULL;
					status = STATUS_INVALID_PARAMETER;
					goto alloc_io_fail;
				}

				if (!isExtended) {
					// -- simple VA: write each chip PTE register directly.
					UINT32 startPte = (UINT32)(baseDevVa >> PAGE_SHIFT);
					if (startPte + pageCount > pDC->PageTableSize) {
						DbgPrint("[ALLOC_IO] slot %d simple-VA out of range: PTE[%u..%u] > %u\n",
							i, startPte, startPte + pageCount - 1, pDC->PageTableSize);
						MmUnmapLockedPages(slot->UserVa, slot->Mdl);
						IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
						MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
						slot->UserVa = NULL;
						status = STATUS_INVALID_PARAMETER;
						goto alloc_io_fail;
					}

					WdfSpinLockAcquire(pDC->PageTableLock);
					for (j = 0; j < pageCount; j++) {
						UINT64 pagePa = basePa + ((UINT64)j << PAGE_SHIFT);
						apex_write_register(
							pDC->Bar2BaseAddress,
							APEX_REG_PAGE_TABLE + ((startPte + j) * 8),
							pagePa | 0x1ULL
						);
					}
					WdfSpinLockRelease(pDC->PageTableLock);
					DbgPrint("[ALLOC_IO] slot %d simple PTE[%u..%u] = (PA 0x%llx + i*4K) | 1\n",
						i, startPte, startPte + pageCount - 1, basePa);
				}
				else {
					// -- extended VA: write L2 PT entries directly into ExtPool.
					//    L1 entries (chip PTE[6144..8191]) were pre-filled at
					//    ApexPageTableInit pointing at the ExtPool sub-regions.
					//    Slot buffer is contiguous so PFN[j] = (basePa>>12) + j.
					//    Buffer may span multiple 2MB L1 regions (e.g. exe1
					//    PARAM_DATA = 6.14MB ≈ 4 regions) — iterate page-by-page
					//    and dispatch each into its own L1 sub-table.
					if (pDC->ExtPoolKva == NULL) {
						DbgPrint("[ALLOC_IO] slot %d EXT VA=0x%llx ExtPool not init\n",
							i, baseDevVa);
						MmUnmapLockedPages(slot->UserVa, slot->Mdl);
						IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
						MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
						slot->UserVa = NULL;
						status = STATUS_DEVICE_NOT_READY;
						goto alloc_io_fail;
					}

					{
						UINT32 firstL1 = (UINT32)((baseDevVa >> 21) & 0x1FFF);
						UINT32 lastL1  = (UINT32)(((baseDevVa +
							((UINT64)(pageCount - 1) << PAGE_SHIFT)) >> 21) & 0x1FFF);
						if (lastL1 >= 2048u) {
							DbgPrint("[ALLOC_IO] slot %d EXT VA=0x%llx spans L1[%u..%u] beyond pool (max 2047)\n",
								i, baseDevVa, firstL1, lastL1);
							MmUnmapLockedPages(slot->UserVa, slot->Mdl);
							IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
							MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
							slot->UserVa = NULL;
							status = STATUS_INVALID_PARAMETER;
							goto alloc_io_fail;
						}

						for (j = 0; j < pageCount; j++) {
							UINT64 curVa  = baseDevVa + ((UINT64)j << PAGE_SHIFT);
							UINT32 l1Idx  = (UINT32)((curVa >> 21) & 0x1FFF);
							UINT32 l2Idx  = (UINT32)((curVa >> 12) & 0x1FF);
							UINT64 pagePa = basePa + ((UINT64)j << PAGE_SHIFT);
							UINT64* l2Tab = (UINT64*)((PUCHAR)pDC->ExtPoolKva +
								((SIZE_T)l1Idx << PAGE_SHIFT));
							l2Tab[l2Idx] = pagePa | 0x1ULL;
						}
						DbgPrint("[ALLOC_IO] slot %d EXT VA=0x%llx pages=%u basePA=0x%llx L1[%u..%u] populated\n",
							i, baseDevVa, pageCount, basePa, firstL1, lastL1);
					}
				}

				slot->DeviceVa = baseDevVa;
			}

			*req[i].outUserVa = (UINT64)slot->UserVa;
			*req[i].outPa = (UINT64)pa.QuadPart;
			
			DbgPrint("[ALLOC_IO] slot %d: KVA=%p UserVA=%p PA=0x%llx DeviceVA=0x%llx size=0x%llx\n",
				i, slot->Kva, slot->UserVa, (UINT64)pa.QuadPart, slot->DeviceVa, (UINT64)size4k);
		}

		bytesReturned = sizeof(*pOut);
		status = STATUS_SUCCESS;
		break;

	alloc_io_fail:
		DbgPrint("[ALLOC_IO] alloc_io_fail cleanup!\n");
		for (i = 0; i < IO_SLOT_COUNT; i++) {
			ALLOC_IO_SLOT* slot = &pDC->IOSlots[i];
			if (slot->UserVa) { MmUnmapLockedPages(slot->UserVa, slot->Mdl); slot->UserVa = NULL; }
			if (slot->Mdl) { IoFreeMdl(slot->Mdl); slot->Mdl = NULL; }
			if (slot->Kva) {
				ApexPageTableUnmap(device, slot->DeviceVa, slot->Size);
				MmFreeContiguousMemory(slot->Kva);
				slot->Kva = NULL; slot->Size = 0; slot->DeviceVa = 0;
			}
		}

		break;
	}

	case IOCTL_FREE_IO_BUFFERS: 
	{
		PDEVICE_CONTEXT pDC = DeviceGetContext(device);
		int i;
		for (i = 0; i < IO_SLOT_COUNT; i++) {
			ALLOC_IO_SLOT* slot = &pDC->IOSlots[i];
			if (slot->UserVa) { MmUnmapLockedPages(slot->UserVa, slot->Mdl); slot->UserVa = NULL; }
			if (slot->Mdl) { IoFreeMdl(slot->Mdl); slot->Mdl = NULL; }
			if (slot->Kva) {
				ApexPageTableUnmap(device, slot->DeviceVa, slot->Size);
				MmFreeContiguousMemory(slot->Kva);
				slot->Kva = NULL; slot->Size = 0; slot->DeviceVa = 0;
			}
		}
		status = STATUS_SUCCESS;
		break;
	}

	default:
		DbgPrint("[%s] Unknown IOCTL: 0x%x\n", __FUNCTION__, IoControlCode);
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
