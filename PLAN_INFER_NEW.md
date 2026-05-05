# PLAN — IOCTL_INFER_NEW (필수 동작만 남긴 슬림 INFER)

## 0. 한 줄 요약

기존 `IOCTL_INFER` 는 디버그/검증 dump 가 700+ 줄. 이 plan 은 **CSR 을 실제로 건드리거나 OS 자원을 lock 하는 동작만** 남긴 골격. simple VA 가정 (`add_model_test_console.cpp` 가 이미 simple VA 로 바뀐 상태) + bounce buffer 없음. PARAM 모델은 일단 미지원 (필요해지면 별도 IOCTL).

---

## 1. Public.h 추가

`IOCTL_INFER_INFO` 구조체는 그대로 재사용 (호환성). IOCTL 코드만 새로 발급.

```c
// IOCTL_INFER 와 동일 input struct 사용. 차이는 driver-side 구현 분량.
//   - 디버그 dump 없음
//   - simple VA 만 처리 (extended/bounce 분기 제거)
//   - PARAM 미지원 (single descriptor)
//   - 완료 판정: SC_HOST_INT_COUNT delta
#define IOCTL_INFER_NEW \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)
```

---

## 2. 9 단계 흐름

| # | 단계 | 책임 |
|---|---|---|
| 1 | input 검증 + retrieve | `WdfRequestRetrieveInputMemory` |
| 2 | input/output/scratch 페이지 lock | `IoAllocateMdl` + `MmProbeAndLockPages` |
| 3 | MDL & 완료 이벤트 초기화 | `pDC->Infer*Mdl` save, `KeClearEvent` |
| 4 | input 페이지 → chip PTE | simple VA: `apex_write_register` 루프 |
| 5 | output 페이지 → chip PTE | 동일 |
| 6 | scratch 페이지 → chip PTE | 동일 (있을 때만) |
| 7 | 모든 engine kRun | RUN_CONTROL=1 일괄 write |
| 8 | descriptor submit | ring write + KeMemoryBarrier + tail register |
| 9 | 완료 대기 + cleanup | `KeWaitForSingleObject` → polling fallback → `MmUnlockPages` |

각 단계가 정확히 한 가지 일만 한다는 점이 중요. 한 단계라도 빼면 chip 이 멈춘다.

---

## 3. case 본체 — Queue.c 에 추가

`case IOCTL_INFER_WITH_PARAM:` 블록 끝난 다음에 둔다.

```c
case IOCTL_INFER_NEW:
{
    PDEVICE_CONTEXT  pDC  = DeviceGetContext(device);
    PVOID            bar2 = pDC->Bar2BaseAddress;
    WDFMEMORY        inMem;
    IOCTL_INFER_INFO *pIn = NULL;
    PMDL inMdl = NULL, outMdl = NULL, scMdl = NULL;
    UINT32 i;

    // [1] retrieve
    if (InputBufferLength < sizeof(IOCTL_INFER_INFO)) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    status = WdfRequestRetrieveInputMemory(Request, &inMem);
    if (!NT_SUCCESS(status)) break;
    pIn = (IOCTL_INFER_INFO *)WdfMemoryGetBuffer(inMem, NULL);

    // simple VA 만 — extended bit 셋이면 즉시 거부
    if ((pIn->InputDeviceVA  & (1ULL << 63)) ||
        (pIn->OutputDeviceVA & (1ULL << 63)) ||
        ((pIn->ScratchSize > 0) && (pIn->ScratchDeviceVA & (1ULL << 63)))) {
        DbgPrint("[INFER_NEW] extended VA not supported in this path\n");
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    // [2] lock pages
    inMdl = IoAllocateMdl((PVOID)pIn->InputImageAddr,
                          (ULONG)pIn->InputImageSize, FALSE, FALSE, NULL);
    if (!inMdl) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
    __try { MmProbeAndLockPages(inMdl, UserMode, IoReadAccess); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(inMdl);
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    outMdl = IoAllocateMdl((PVOID)pIn->OutputBufferAddr,
                           (ULONG)pIn->OutputBufferSize, FALSE, FALSE, NULL);
    if (!outMdl) {
        MmUnlockPages(inMdl); IoFreeMdl(inMdl);
        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
    }
    __try { MmProbeAndLockPages(outMdl, UserMode, IoWriteAccess); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        MmUnlockPages(inMdl); IoFreeMdl(inMdl);
        IoFreeMdl(outMdl);
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    if (pIn->ScratchAddr != 0 && pIn->ScratchSize > 0) {
        scMdl = IoAllocateMdl((PVOID)pIn->ScratchAddr,
                              (ULONG)pIn->ScratchSize, FALSE, FALSE, NULL);
        if (!scMdl) {
            MmUnlockPages(inMdl); IoFreeMdl(inMdl);
            MmUnlockPages(outMdl); IoFreeMdl(outMdl);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        __try { MmProbeAndLockPages(scMdl, UserMode, IoWriteAccess); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            IoFreeMdl(scMdl); scMdl = NULL;
            MmUnlockPages(inMdl); IoFreeMdl(inMdl);
            MmUnlockPages(outMdl); IoFreeMdl(outMdl);
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    // [3] save MDL + reset completion event
    pDC->InferInputMdl   = inMdl;
    pDC->InferOutputMdl  = outMdl;
    pDC->InferScratchMdl = scMdl;
    KeClearEvent(&pDC->InferCompleteEvent);

    // [4] input → PTE
    {
        UINT32 startPte = (UINT32)(pIn->InputDeviceVA >> PAGE_SHIFT);
        UINT32 pages    = (UINT32)((pIn->InputImageSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
        PPFN_NUMBER pfn = MmGetMdlPfnArray(inMdl);
        WdfSpinLockAcquire(pDC->PageTableLock);
        for (i = 0; i < pages; i++) {
            UINT64 pa = ((UINT64)pfn[i]) << PAGE_SHIFT;
            apex_write_register(bar2,
                APEX_REG_PAGE_TABLE + ((startPte + i) * 8), pa | 0x1);
        }
        WdfSpinLockRelease(pDC->PageTableLock);
    }

    // [5] output → PTE
    {
        UINT32 startPte = (UINT32)(pIn->OutputDeviceVA >> PAGE_SHIFT);
        UINT32 pages    = (UINT32)((pIn->OutputBufferSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
        PPFN_NUMBER pfn = MmGetMdlPfnArray(outMdl);
        WdfSpinLockAcquire(pDC->PageTableLock);
        for (i = 0; i < pages; i++) {
            UINT64 pa = ((UINT64)pfn[i]) << PAGE_SHIFT;
            apex_write_register(bar2,
                APEX_REG_PAGE_TABLE + ((startPte + i) * 8), pa | 0x1);
        }
        WdfSpinLockRelease(pDC->PageTableLock);
    }

    // [6] scratch → PTE (있을 때만)
    if (scMdl) {
        UINT32 startPte = (UINT32)(pIn->ScratchDeviceVA >> PAGE_SHIFT);
        UINT32 pages    = (UINT32)((pIn->ScratchSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
        PPFN_NUMBER pfn = MmGetMdlPfnArray(scMdl);
        WdfSpinLockAcquire(pDC->PageTableLock);
        for (i = 0; i < pages; i++) {
            UINT64 pa = ((UINT64)pfn[i]) << PAGE_SHIFT;
            apex_write_register(bar2,
                APEX_REG_PAGE_TABLE + ((startPte + i) * 8), pa | 0x1);
        }
        WdfSpinLockRelease(pDC->PageTableLock);
    }

    // [7] 모든 engine kRun. tile_config0 도 다시 박아준다 (engine 이 거부 방지).
    apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
    KeStallExecutionProcessor(50);

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
    KeStallExecutionProcessor(1000);   // 1 ms settle

    // [8] descriptor submit (single INFER, no PARAM)
    {
        typedef struct {
            UINT64 address;
            UINT32 size_in_bytes;
            UINT32 reserved;
        } HOST_QUEUE_DESC;

        HOST_QUEUE_DESC *ring = (HOST_QUEUE_DESC *)pDC->DescRingBase;
        UINT32 slot = pDC->DescRingTail % 256;

        ring[slot].address       = pIn->BitstreamDeviceVA;
        ring[slot].size_in_bytes = (UINT32)pIn->BitstreamSize;
        ring[slot].reserved      = 0;
        KeMemoryBarrier();           // ring write 가 chip 보다 먼저 보이도록

        pDC->DescRingTail++;
        apex_write_register(bar2, APEX_REG_INSTR_QUEUE_TAIL, pDC->DescRingTail);
    }

    // SC_HOST_INT_COUNT 스냅샷 — 완료 판정 기준선
    pDC->LastScHostIntCount =
        apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);

    // [9] 완료 대기. 1차: ISR 깨움(50 ms). 2차: SC_HOST_INT_COUNT 폴링(최대 5 s).
    {
        LARGE_INTEGER t1; t1.QuadPart = -500000LL; // 50ms
        status = KeWaitForSingleObject(&pDC->InferCompleteEvent,
                                       Executive, KernelMode, FALSE, &t1);
    }
    if (status == STATUS_TIMEOUT) {
        UINT64 pre = pDC->LastScHostIntCount;
        int p;
        for (p = 0; p < 1000; p++) {                 // 5 s
            UINT64 cur = apex_read_register(bar2, APEX_REG_SC_HOST_INT_COUNT);
            UINT32 sc  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
            UINT32 of  = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
            // SC_HOST 카운트가 증가 + SCALAR/OUTFEED kIdle(0) or kHalted(4)
            if (cur > pre && (sc == 0 || sc == 4) && (of == 0 || of == 4)) {
                KeSetEvent(&pDC->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
                status = STATUS_SUCCESS;
                break;
            }
            { LARGE_INTEGER d; d.QuadPart = -10000LL;       // 1ms
              KeDelayExecutionThread(KernelMode, FALSE, &d); }
        }
        if (status == STATUS_TIMEOUT) {
            DbgPrint("[INFER_NEW] timeout — SC_HOST stuck. HIB_ERROR=0x%llx\n",
                apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS));
        }
    }

    // 페이지 unlock — 성공/실패 무관하게 항상.
    if (pDC->InferInputMdl)   { MmUnlockPages(pDC->InferInputMdl);   IoFreeMdl(pDC->InferInputMdl);   pDC->InferInputMdl   = NULL; }
    if (pDC->InferOutputMdl)  { MmUnlockPages(pDC->InferOutputMdl);  IoFreeMdl(pDC->InferOutputMdl);  pDC->InferOutputMdl  = NULL; }
    if (pDC->InferScratchMdl) { MmUnlockPages(pDC->InferScratchMdl); IoFreeMdl(pDC->InferScratchMdl); pDC->InferScratchMdl = NULL; }

    break;
}
```

---

## 4. user-side 변경 (`add_model_test_console.cpp`)

지금 `inferIoctl = useInferWithParam ? IOCTL_INFER_WITH_PARAM : IOCTL_INFER;` 한 줄을:

```cpp
DWORD inferIoctl = IOCTL_INFER_NEW;        // 일단 STAND_ALONE 모델만
```

또는 fallback 형태로:

```cpp
DWORD inferIoctl =
    useInferWithParam ? IOCTL_INFER_WITH_PARAM   // PARAM 모델은 기존 경로
                      : IOCTL_INFER_NEW;          // STAND_ALONE 은 슬림 경로
```

`IOCTL_INFER_INFO` 채우는 부분은 그대로. struct 동일.

---

## 5. 의도적으로 뺀 것 (디버깅 시 다시 켜기)

기존 `IOCTL_INFER` 에서 **제거된 동작 목록**. 문제 생기면 이 중에서 해당 항목만 골라 다시 추가.

| 카테고리 | 제거한 것 |
|---|---|
| **로그 dump** | `[BS-DUMP]`, `[BS-SCAN]`, `[INPUT-CHK]` 8-byte first/last + xor-fold, `[CSR@pre/post]` 일괄 sweep |
| **상태 진단** | pre/post `[CREDITS]`, `[AXI@pre/post]`, `[ARB-W]`/`[ARB-AT]`, `[SCAN-DescRing/StatusBlock/PageTable/...]`, `[INPUT-VERIFY]` xor 비교 |
| **PCI** | `npudriverDumpPciCommand` / `npudriverDumpPciAer` |
| **20 ms 조기 폴링** | `[POLL@*ms] INFEED/OUTFEED/AVDATA/HIB_ERR/FAULT_VA` 첫 20 tick |
| **sentinel fill** | output buffer 0xCC pre-fill, DescRing/StatusBlock 0xA5 pre-fill |
| **PTE readback 검증** | input/output PTE 4 개 readback 비교 |
| **engine pre/post-wake snapshot** | RUN_STATUS dump 두 번 |
| **HIB_ERROR 비트 디코드** | bit5/7/9 명시 |
| **PARAM 분기** | `IOCTL_INFER_WITH_PARAM` 의 PARAM_CACHE descriptor 동시 발행 |
| **extended VA 분기** | `ApexExtMapBuffer` + bounce buffer (`ApexAllocOutputBounce`) |

총 ~600 줄 → ~150 줄 수준으로 슬림화.

---

## 6. 검증 체크리스트

빌드 후 단계별로 확인.

- [ ] `IOCTL_INFER_NEW` 등록되고 dispatch 됨 (DbgPrint `[%s] IOCTL: 0x%x` 에서 0x808 보이는지)
- [ ] [4]/[5]/[6] PTE 루프 각각 일관된 `startPte` 와 `pageCount` 로 도는지 (Verifier on)
- [ ] [8] descriptor submit 후 `INSTR_QUEUE_TAIL` 이 1 만큼 올라간 값으로 readback 됨
- [ ] [9] 1차 wait (50 ms) 내에 ISR 깨움이 들어와 `KeWaitForSingleObject` 가 SUCCESS 리턴 (정상 path)
- [ ] 폴링 fallback 진입했을 때 `SC_HOST_INT_COUNT` 가 pre 보다 증가하면 SUCCESS 로 빠져나옴
- [ ] 호출 직후 user-mode 의 `pOutputBuf` 16 byte 가 add_int8 golden 과 일치 (=PASS)
- [ ] 실패 시 `HIB_ERROR` 한 줄로 충분한지, 더 깊이 봐야 하면 §5 항목에서 골라 임시 복원
- [ ] cleanup 경로: 모든 break/error 분기가 lock 한 MDL 을 unlock 하는지 (Verifier 의 page-leak 검사 통과)

---

## 7. 작업 순서 (실제 type 칠 때)

1. `include/Public.h` — `IOCTL_INFER_NEW` 정의
2. `npu_driver/Queue.c` — 위 case 본체 추가
3. 빌드 → 드라이버 재서명 → 재로드
4. `add_model_test_console.cpp` `inferIoctl` 한 줄 교체
5. 빌드 → 실행
6. golden 일치 안 하면 §5 표 보고 한 항목씩 다시 켜며 좁혀들어감

---

## 8. 함정

- **completion 판정**: ISR 만 믿으면 안 됨. ISR 이 깨워지지 않는 케이스 (chip halt, MSI-X miss) 가 종종 있어서 폴링 fallback 필수. SC_HOST_INT_COUNT 는 SCALAR 의 host_interrupt 0 opcode 가 OUTFEED drain barrier 뒤에 박혀있어 가장 신뢰 가능.
- **engine kRun 누락**: `INFEED_RUN_CONTROL=1` 안 박으면 input DMA 가 시작 안 됨. 16 byte 가 안 빨려가서 output 도 0 으로 끝남. 모든 engine 한 번에 박는 게 안전.
- **`KeMemoryBarrier`**: ring write → tail register write 사이에 반드시. 빠지면 chip 이 stale descriptor 읽어서 random VA 로 fetch 시도.
- **`KeStallExecutionProcessor(1000)` 대신 `KeDelayExecutionThread`**: stall 은 spinlock 안에서도 쓸 수 있는 busy-wait. settle 용으론 충분하지만 길게 stall 하면 코어 잡아먹음. 1 ms 정도가 한계.
- **PFN 배열 caching 금지**: `MmGetMdlPfnArray` 결과는 MDL 살아있는 동안만 유효. unlock 후 접근하면 BSOD.
- **InferCompleteEvent reset 시점**: descriptor submit 전, MDL 저장 후. 순서 어기면 직전 inference 의 stale signal 잡을 수 있음.
- **PageTableLock 중첩 금지**: 한 IOCTL 안에서 lock 세 번 acquire/release. 안에서 다른 PTE 함수 호출하면 deadlock 위험 (현재 코드는 인라인이라 OK).

---

## 9. 이게 통과하면 다음

- add_int8 byte-exact 통과 → SSD 디버깅 oracle 확보 (`add_int8_oracle_strategy` 메모).
- 통과 못 하면 §5 항목에서 가장 의심되는 줄부터 한 개씩 켜서 좁힘. 우선순위 추천:
  1. `[CSR@pre/post]` sweep — 어느 register 가 변했는지 가장 빨리 보임
  2. `[INPUT-CHK] xor-fold` 비교 — chip 이 input 영역을 침범하지 않았는지 확인
  3. `[POLL@*ms]` 20-tick 조기 폴링 — INFEED 가 kRun 으로 진입했는지
  4. `[BS-SCAN]` patch hit count — bitstream patch 빠진 거 있는지
