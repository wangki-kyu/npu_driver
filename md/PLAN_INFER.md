# IOCTL_INFER + ISR/DPC 구현 계획

## Context

gasket-driver 분석 결과:
- 실제 gasket은 추론 실행을 userspace(libedgetpu)가 BAR2에 직접 MMIO write로 트리거
- 드라이버는 페이지 테이블 + 인터럽트만 담당
- 완료: MSI-X 인터럽트 → SC_HOST_0~3 (인덱스 4~7) → userspace

우리 방식: 드라이버가 IOCTL_INFER 수신 시 RunControl 레지스터 직접 write → 추론 시작 → ISR/DPC 완료 처리

현재 상태:
- IOCTL_INFER case 스켈레톤만 있음 (Queue.c:106)
- Interrupts[1] 필드 선언만 있음, WdfInterruptCreate 없음
- ISR/DPC 미구현

---

## libedgetpu 분석 결과 (핵심 발견)

입력/출력 device VA는 레지스터에 직접 쓰지 않음.
**edgetpu.tflite 내부 인스트럭션 비트스트림의 MOVI 명령어 immediate 필드에 패치**해야 함.

- `LinkInputAddress()` → `Description_BASE_ADDRESS_INPUT_ACTIVATION` 필드를 찾아 상/하위 32비트 write
- `LinkOutputAddress()` → 동일 방식으로 출력 VA 패치
- 패치된 인스트럭션 버퍼를 Instruction Queue descriptor에 넣어 APEX에 전달
- Instruction Queue tail 레지스터: `0x485a8`

입력/출력 버퍼 매핑 시 **Extended VA** 영역 사용:
- 모델: simple VA (0x0 ~)
- 입력/출력: extended VA (`0x8000000000000000` ~)

커널 드라이버에서 FlatBuffer 파싱은 부적절 → **hpp로 분리**

---

## 역할 분담

| 역할 | 담당 |
|------|------|
| FlatBuffer 파싱 + VA 패치 | **`include/apex_model.hpp`** (userspace hpp) |
| 페이지 테이블 관리 | 드라이버 (이미 구현) |
| 이미지/출력 Extended VA PTE 등록 | 드라이버 (IOCTL_INFER에서) |
| Instruction Queue 설정 (0x485a8) | 드라이버 (IOCTL_INFER에서) |
| ISR/DPC 완료 처리 | 드라이버 (신규 구현) |

---

## 수정/생성 파일 목록

| 파일 | 작업 |
|------|------|
| `include/apex_model.hpp` | **신규** - FlatBuffer 파싱 + 입출력 VA 패치 헬퍼 |
| `npu_driver/Device.h` | DEVICE_CONTEXT에 InferInputMdl, InferOutputMdl, InferCompleteEvent 추가 + ISR/DPC 선언 |
| `npu_driver/Device.c` | WdfInterruptCreate + KeInitializeEvent + ISR/DPC 구현 |
| `npu_driver/Queue.c` | IOCTL_INFER 핸들러 구현 (Extended VA PTE 등록 + Instr Queue 포함) |

---

## Step 1: Device.h

DEVICE_CONTEXT에 추가:
```c
PMDL  InferInputMdl;          // DPC에서 unlock
PMDL  InferOutputMdl;
KEVENT InferCompleteEvent;    // IOCTL 대기, DPC가 signal
```

선언 추가:
```c
EVT_WDF_INTERRUPT_ISR npudriverEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC npudriverEvtInterruptDpc;
```

---

## Step 2: Device.c

### npudriverCreateDevice에 추가 (WdfDeviceCreate 성공 후)

```c
WDF_INTERRUPT_CONFIG interruptConfig;
WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
    npudriverEvtInterruptIsr,
    npudriverEvtInterruptDpc);
WdfInterruptCreate(device, &interruptConfig,
    WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->Interrupts[0]);

KeInitializeEvent(&deviceContext->InferCompleteEvent, NotificationEvent, FALSE);
deviceContext->InferInputMdl  = NULL;
deviceContext->InferOutputMdl = NULL;
```

### ISR

```c
BOOLEAN npudriverEvtInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(WdfInterruptGetDevice(Interrupt));

    UINT64 pending = apex_read_register(pDevContext->Bar2BaseAddress,
                                        APEX_REG_WIRE_INT_PENDING);
    if (pending == 0) return FALSE;

    apex_write_register(pDevContext->Bar2BaseAddress,
                        APEX_REG_WIRE_INT_PENDING, pending);
    WdfInterruptQueueDpc(Interrupt);
    return TRUE;
}
```

### DPC

```c
VOID npudriverEvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(WdfInterruptGetDevice(Interrupt));

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
    KeSetEvent(&pDevContext->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
}
```

---

## Step 3: Queue.c - IOCTL_INFER 핸들러

```c
case IOCTL_INFER:
{
    WDFMEMORY inputMemory;
    IOCTL_INFER_INFO *pInput = NULL;
    PMDL inputImageMdl = NULL, outputBufferMdl = NULL;
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);
    PVOID bar2 = pDevContext->Bar2BaseAddress;
    LARGE_INTEGER timeout;

    if (InputBufferLength < sizeof(IOCTL_INFER_INFO)) {
        status = STATUS_INVALID_PARAMETER; break;
    }

    // 1. 입력 구조체 읽기 (METHOD_IN_DIRECT, 자동 probe됨)
    status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
    if (!NT_SUCCESS(status)) break;
    pInput = (IOCTL_INFER_INFO *)WdfMemoryGetBuffer(inputMemory, NULL);

    // 2. 입력 이미지 probe & lock
    inputImageMdl = IoAllocateMdl((PVOID)pInput->InputImageAddr,
                                   (ULONG)pInput->InputImageSize, FALSE, FALSE, NULL);
    if (!inputImageMdl) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
    __try { MmProbeAndLockPages(inputImageMdl, UserMode, IoReadAccess); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(inputImageMdl); status = STATUS_INVALID_PARAMETER; break;
    }

    // 3. 출력 버퍼 probe & lock
    outputBufferMdl = IoAllocateMdl((PVOID)pInput->OutputBufferAddr,
                                     (ULONG)pInput->OutputBufferSize, FALSE, FALSE, NULL);
    if (!outputBufferMdl) {
        MmUnlockPages(inputImageMdl); IoFreeMdl(inputImageMdl);
        status = STATUS_INSUFFICIENT_RESOURCES; break;
    }
    __try { MmProbeAndLockPages(outputBufferMdl, UserMode, IoWriteAccess); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        MmUnlockPages(inputImageMdl); IoFreeMdl(inputImageMdl);
        IoFreeMdl(outputBufferMdl); status = STATUS_INVALID_PARAMETER; break;
    }

    // 4. MDL 저장 (DPC에서 사용)
    pDevContext->InferInputMdl  = inputImageMdl;
    pDevContext->InferOutputMdl = outputBufferMdl;
    KeClearEvent(&pDevContext->InferCompleteEvent);

    // 5. 추론 시작 시퀀스 (Scalar → Infeed → Tile → Outfeed)
    apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL,        1);
    apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL,         1);
    apex_write_register(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL,  1);
    apex_write_register(bar2, APEX_REG_TILE_CONFIG0,               0x7F);
    apex_write_register(bar2, APEX_REG_TILE_OP_RUN_CONTROL,        1);
    apex_write_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL, 1);
    apex_write_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL, 1);
    apex_write_register(bar2, APEX_REG_OUTFEED_RUN_CONTROL,        1);

    // 6. ISR/DPC 완료 대기 (1초 타임아웃)
    timeout.QuadPart = -10000000LL;
    status = KeWaitForSingleObject(&pDevContext->InferCompleteEvent,
                                    Executive, KernelMode, FALSE, &timeout);

    // 7. 타임아웃 시 cleanup
    if (status == STATUS_TIMEOUT) {
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
    }
    break;
}
```

---

## 전체 흐름

```
npu_test_console
  │
  ├─ apex_model.hpp: LoadAndPatchModel()
  │    ├─ edgetpu.tflite 로드
  │    ├─ FlatBuffer 파싱 → field_offsets
  │    ├─ 입력 device VA (0x8000000000000000) 패치
  │    └─ 출력 device VA 패치
  │
  ├─ IOCTL_MAP_BUFFER(patched_model)   ← 패치된 모델 등록
  │
  ├─ IOCTL_INFER(image_addr, out_addr)
  │    ├─ image probe & lock → Extended VA PTE 등록
  │    ├─ output probe & lock → Extended VA PTE 등록
  │    ├─ Instruction Queue descriptor write (0x485a8)
  │    ├─ RunControl 레지스터 write
  │    └─ KeWaitForSingleObject (대기)
  │              │
  │         [APEX 추론 중...]
  │              │
  │         MSI-X 인터럽트 (SC_HOST_0)
  │              │
  │         ISR: WIRE_INT_PENDING 확인 → 클리어 → DPC 예약
  │              │
  │         DPC: MDL unlock → KeSetEvent
  │              │
  │    └─ 대기 해제 → STATUS_SUCCESS 반환
  │
  └─ IOCTL_UNMAP_BUFFER(model)         ← 이미 구현됨
```

---

## 인터럽트 관련 정보 (gasket-driver에서 확인)

| 항목 | 값 |
|------|-----|
| 인터럽트 총 개수 | 13개 (MSI-X) |
| 추론 완료 인터럽트 | SC_HOST_0~3 (인덱스 4~7) |
| INTVECCTL 레지스터 | APEX_REG_SC_HOST_INTVECCTL (0x46038) |
| 완료 확인 방법 | APEX_REG_WIRE_INT_PENDING (0x48778) |

---

## 검증 DbgPrint 순서

```
[npudriverEvtIoDeviceControl] IOCTL_INFER received
[npudriverEvtIoDeviceControl] Inference started
[npudriverEvtInterruptIsr] pending=0x...
[npudriverEvtInterruptDpc] Inference complete, unlocking pages
[npudriverEvtIoDeviceControl] Inference done, status=0x0
```
