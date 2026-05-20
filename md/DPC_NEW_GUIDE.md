# `npudriverEvtInterruptDpcNew` 구현 가이드

IRQ-only 완료 경로용 새 DPC. 기존 `npudriverEvtInterruptDpc` (`Device.c:1374`) 는 그대로 두고 **새 함수**를 추가, `WDF_INTERRUPT_CONFIG_INIT` 의 DPC 인자만 교체한다.

> **전제 (중요):** IOCTL_INFER_NEW 는 `IOCTL_ALLOC_IO_BUFFERS` 가 input/output/scratch 를 driver-allocated contiguous (nonpaged) 로 미리 잡아둔 슬롯을 재사용한다. 따라서 새 DPC 는 **MDL 을 lock/unlock 하지 않고, IoFreeMdl 도 하지 않고, bounce 카피도 없다.** 정상 완료 시 책임은 오직 (a) 게이트 통과 확인 → (b) `KeSetEvent` 두 가지뿐. 기존 DPC 의 unlock/copy 로직은 IOCTL_INFER (legacy) 전용이므로 그쪽은 손대지 않는다.

목적은 두 가지:
1. 기존 DPC 의 진단/디버깅 코드 + IOCTL_INFER 전용 unlock/copy 경로를 걷어내고 **새 경로에 필요한 최소 동작** 만 남긴다.
2. SC_HOST_0 와 OUTFEED 물리 drain 사이의 race 를 닫는 **DPC 내부 settle 폴링** 을 추가해, IOCTL 측의 폴링 fallback 없이도 안전하게 `InferCompleteEvent` 를 set 할 수 있게 만든다.

---

## 1. 설계 원칙

| 원칙 | 이유 |
|---|---|
| `KeSetEvent(InferCompleteEvent)` 는 **DPC 단독 권한** | 이 시그널 = "사용자가 결과 읽어가도, IOCTL_FREE_IO_BUFFERS 호출해도 안전 = OUTFEED 가 마지막 바이트까지 contiguous 슬롯에 다 썼다" 를 단언하는 행위. IOCTL 측에서 절대 set 하면 안 됨. |
| **allIdle 게이트 필수** | SCALAR 의 `host_interrupt 0` 은 OUTFEED drain 보다 먼저 호스트에 도달할 수 있음 (`project_sc_host_outfeed_race.md`). SC_HOST_0 단독으로 KeSetEvent 하면 DMA-after-free 가능: 사용자가 IOCTL_FREE_IO_BUFFERS 로 슬롯 해제 → chip 의 뒤늦은 W TLP 가 freed contiguous block 의 (재할당된) 페이지를 침범 → `HIB_ERROR=0x1` + FATAL_ERR (최악의 경우 BSOD). |
| **lock/unlock 안 함** | IO 슬롯은 이미 ALLOC_IO_BUFFERS 가 nonpaged contiguous 로 잡아뒀고 chip PTE 도 그때 박혔음. DPC 는 슬롯에 손대지 않는다 — `MmUnlockPages` / `IoFreeMdl` / `RtlCopyMemory(bounce→user)` 호출 일체 없음. |
| **IOCTL 측 폴링 금지** | 새 IOCTL_INFER_NEW 는 `KeWaitForSingleObject(InferCompleteEvent, 5s)` 한 번만. 타임아웃 = 진짜 실패. SC_HOST_INT_COUNT delta 로 강제 KeSetEvent 하던 fallback 경로는 새 경로에 등장 금지. |
| **두 갈래 premature 분기** | (a) SC_HOST_0 봤는데 not allIdle → DPC 내부 settle 폴링(2ms cap). (b) SC_HOST_0 못 봤고 not terminal → return, 다음 MSI-X 대기. **무조건 KeSetEvent 절대 금지.** |

---

## 2. 기존 DPC 에서 가져갈 것 / 버릴 것

원본은 `Device.c:1374-1807`.

### 가져갈 코드 (functional)

| 블록 | 원본 라인 | 용도 |
|---|---|---|
| FATAL_ERR 감지 | 1479-1481 | 칩 fatal error 면 게이트 우회해서 완료시켜야 함 (deadlock 방지) |
| Run-status 읽기 (SC/IN/OUT/AV) | 1503-1507 | allIdle / halted 판정 |
| PREMATURE 게이트 결정 | 1501-1514 | terminal 판정 + return |
| `KeSetEvent` | 1805-1806 | IOCTL 깨우기 (마지막 동작) |

### 버릴 코드

| 블록 | 원본 라인 | 사유 |
|---|---|---|
| `[DPC-DIAG]` 레지스터 dump | 1402-1430 | 디버깅용 |
| `[DPC-AXI]` write-channel counters | 1439-1446 | 디버깅용 |
| `[DPC-CREDITS]` HIB credits | 1451-1458 | 디버깅용 |
| `[DPC-SB]` StatusBlock dump | 1471-1475 | 디버깅용 |
| `[DPC-PTE]` PTE readback (simple/extended) | 1520-1569 | 디버깅용 |
| `[DPC-OUT]` 출력 버퍼 full scan + tail dump | 1571-1644 | 디버깅용 (full scan 은 무거움) |
| `[DPC-SCAN-*]` DescRing/StatusBlock/INFER/Input/PARAM scan | 1652-1745 | 디버깅용 |
| **입력 MDL 언락** | 1747-1752 | **INFER_NEW 슬롯은 IOCTL 가 lock 하지 않음 — 언락 대상 자체 없음** |
| **출력 MDL 언락 + bounce 카피** | 1754-1796 | **INFER_NEW 는 simple VA 만 + driver-allocated 슬롯 → bounce 자체가 없음** |
| **스크래치 MDL 언락** | 1798-1803 | **상동** |
| 진입 / 단계별 `DbgPrint` | 1382, 1751, 1795, 1802, 1806 등 | 정상 경로 noise |

> 새 DPC 에는 **DbgPrint 를 한두 줄만** 남긴다 (premature defer 1줄, terminal completion 1줄). 그 외 dump 류는 전부 제거.

---

## 3. 핵심 로직: settle 폴링

기존 DPC 는 not-terminal 이면 무조건 `return` (기존 1510-1514). 하지만 **SC_HOST_0 가 이미 set 됐는데 OUT_RUN_STATUS 만 1ms 안에 떨어지지 않는** 케이스가 빈번하다 (`project_sc_host_outfeed_race.md`). 이 경우 다음 MSI-X 가 또 올 거란 보장이 없으므로, DPC 안에서 짧게 기다린다.

```
if (scHostSeen && !terminal) {
    // settle poll: 10us × 200 = 2ms cap
    for (i = 0; i < 200; i++) {
        KeStallExecutionProcessor(10);
        gateOut = read OUT_RUN_STATUS
        gateSc  = read SC_RUN_STATUS
        gateIn  = read IN_RUN_STATUS
        gateAv  = read AVDATA_RUN_STATUS
        terminal = allIdle || anyHalted
        if (terminal) break;
    }
    // fall through to MDL unlock + KeSetEvent
}
```

KeStallExecutionProcessor 는 spin-wait (DISPATCH_LEVEL 안전). 통상 µs 단위에 끝나니 200 iter 가 다 돌 일은 거의 없음. 2ms 가 다 찼는데도 not-terminal 이면 — 칩이 비정상이라는 뜻이고, 이때는 다음 MSI-X 도 안 올 가능성이 크니 **settle 실패 로그 + return** 로 처리한다 (다음 MSI-X 가 오면 다시 평가). 무조건 KeSetEvent 하지 말 것.

| 분기 | scHostSeen | terminal | 행동 |
|---|---|---|---|
| 정상 완료 | T | T | `KeSetEvent` (그게 전부) |
| Race (자주 발생) | T | F | settle 폴링 → terminal 되면 `KeSetEvent`, 아니면 return |
| FATAL_ERR | -  | -  | (별도 분기) `KeSetEvent` (deadlock 방지 — IOCTL 측이 USER_HIB_ERROR 로 실패 surface) |
| 정상 premature | F | F | 즉시 return (기록 없이) |
| 비정상 (SC_HOST_0 없는데 terminal) | F | T | return — 다음 MSI-X 대기. 단독으로 완료 판정 안 함 |

> "F+T" (SC_HOST_0 없는데 idle) 은 본 INFER 와 무관한 잔존 상태일 수 있어, 다음 MSI-X 의 SC_HOST_0 를 기다리는 편이 안전.

---

## 4. 함수 스켈레톤 (의사코드)

INFER_NEW 슬롯은 driver-allocated nonpaged contiguous 라 unlock 책임이 없다. DPC 본체는 (게이트 → settle → KeSetEvent) 만 한다.

```c
VOID npudriverEvtInterruptDpcNew(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

    WDFDEVICE       device      = WdfInterruptGetDevice(Interrupt);
    PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);
    PVOID           bar2        = pDevContext->Bar2BaseAddress;

    if (bar2 == NULL) return;

    // 1) ISR 가 OR 해 둔 pending bits 확인
    LONG    seen       = pDevContext->IsrSeenPendingBits;
    BOOLEAN scHostSeen = (seen & APEX_WIRE_BIT_SC_HOST_0) != 0;
    BOOLEAN fatalSeen  = (seen & APEX_WIRE_BIT_FATAL_ERR) != 0;

    // 2) Run-status 스냅샷
    UINT32 gateSc  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
    UINT32 gateIn  = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
    UINT32 gateOut = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
    UINT32 gateAv  = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);

    BOOLEAN allIdle   = (gateSc == 0) && (gateIn == 0) && (gateOut == 0) && (gateAv == 0);
    BOOLEAN anyHalted = (gateSc == 4) || (gateOut == 4);
    BOOLEAN terminal  = allIdle || anyHalted || fatalSeen;

    // 3) settle 폴링: SC_HOST_0 봤는데 아직 idle 아니면 짧게 대기
    if (scHostSeen && !terminal) {
        for (ULONG i = 0; i < 200; i++) {
            KeStallExecutionProcessor(10);   // 10 µs
            gateSc  = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
            gateIn  = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
            gateOut = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
            gateAv  = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
            allIdle   = (gateSc == 0) && (gateIn == 0) && (gateOut == 0) && (gateAv == 0);
            anyHalted = (gateSc == 4) || (gateOut == 4);
            terminal  = allIdle || anyHalted;
            if (terminal) break;
        }
    }

    // 4) 아직도 not-terminal && not-fatal 이면 defer (다음 MSI-X 에 다시 평가)
    if (!terminal && !fatalSeen) {
        DbgPrint("[DPC-NEW] defer: SC=0x%x IN=0x%x OUT=0x%x AV=0x%x scHost=%d\n",
                 gateSc, gateIn, gateOut, gateAv, scHostSeen);
        return; // event unsignaled 유지 — 다음 MSI-X 가 다시 DPC 큐잉
    }

    // 5) 완료 — IO 슬롯은 IOCTL 가 잡아둔 driver-allocated contiguous 라
    //    DPC 가 unlock/free 하지 않는다. 게이트 통과 = 슬롯 내용 유효 보장.
    DbgPrint("[DPC-NEW] complete: SC=0x%x OUT=0x%x scHost=%d fatal=%d halted=%d\n",
             gateSc, gateOut, scHostSeen, fatalSeen, anyHalted);

    // 6) IOCTL 깨우기
    KeSetEvent(&pDevContext->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
}
```

> 함수 본체가 ~30 줄로 줄어든다. 기존 DPC (`Device.c:1374-1807`, ~430 줄) 대비 1/15 수준.

---

## 5. 콜백 교체

### `Device.h:211` 부근 — 선언 추가

기존 `npudriverEvtInterruptDpc` 선언은 두고, 새 함수 선언을 같이 둔다.

```c
EVT_WDF_INTERRUPT_DPC npudriverEvtInterruptDpc;
EVT_WDF_INTERRUPT_DPC npudriverEvtInterruptDpcNew;
```

### `Device.c:88-107` — `WdfInterruptCreate` 4 회 루프

`WDF_INTERRUPT_CONFIG_INIT` 의 두 번째 DPC 인자만 교체:

```c
// Before
WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
    npudriverEvtInterruptIsr,
    npudriverEvtInterruptDpc);

// After (IRQ-only 모드)
WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
    npudriverEvtInterruptIsr,
    npudriverEvtInterruptDpcNew);
```

ISR 은 그대로 유지. ISR 의 `IsrSeenPendingBits` OR-누적 동작이 새 DPC 의 입력이므로 **ISR 도 손대지 않는다**.

### IOCTL_INFER_NEW 측 (Queue.c) — 별도 작업

이번 가이드 범위는 DPC 만. IOCTL_INFER_NEW 핸들러는 `project_infer_new_irq_only.md` 메모대로:
- `KeWaitForSingleObject(InferCompleteEvent, ..., 5s)` 한 번만
- 타임아웃 = 실패 (status 반환). 폴링 fallback 금지.
- 핸들러 안에서 `KeSetEvent(InferCompleteEvent)` 호출 절대 금지 (현 `Queue.c:1008, 1025` 같은 패턴은 새 경로에 등장 안 함)
- 진입 시 `IsrSeenPendingBits = 0` + `KeClearEvent(InferCompleteEvent)` 는 그대로 필요 (`Queue.c:461-463` 참조)

---

## 6. 검증 체크리스트

구현 후 트레이스로 확인할 것:

- [ ] 정상 INFER 1회: `[DPC-NEW] complete:` 1줄만 찍히고 IOCTL 정상 반환
- [ ] settle race 케이스: 1차 ISR 시점 OUT=0x1 인데 settle 폴링이 µs 안에 OUT=0 잡고 정상 완료
- [ ] 진짜 premature (SC_HOST_0 미수신): `[DPC-NEW] defer:` 찍히고 다음 MSI-X 에서 완료
- [ ] FATAL_ERR 발생: deadlock 없이 완료 처리되며 IOCTL 이 USER_HIB_ERROR 로 실패 surface
- [ ] DMA-after-free 재현 안 됨: INFER_NEW → 결과 read → IOCTL_FREE_IO_BUFFERS 사이클 100회에서 `HIB_ERROR=0x1` / FATAL_ERR / chip MMU fault 없음
- [ ] IO 슬롯 손상 없음: 연속 INFER_NEW 후에도 ALLOC_IO_BUFFERS 가 잡아준 슬롯의 KVA/PTE 가 유효 (DPC 가 슬롯에 손대지 않는지 확인)
- [ ] 로그 양: 기존 DPC 대비 정상 케이스 로그가 1/30 이하

---

## 7. 참고

- 원본 DPC: `Device.c:1374-1807`
- ISR (수정 없음): `Device.c:1244-1310`
- WdfInterruptCreate 4회 루프: `Device.c:88-107`
- IOCTL_INFER_NEW 슬롯 lookup 로직: `Queue.c:118` 부근 (`pDC->IOSlots[]`)
- IOCTL_INFER_NEW 슬림 설계: `PLAN_INFER_NEW.md`
- 완료 계약 근거: `memory/project_dpc_completion_contract.md` (legacy IOCTL_INFER 기준 — 새 경로는 unlock 책임 없음에 주의)
- SC_HOST_0 / OUTFEED race 근거: `memory/project_sc_host_outfeed_race.md`
- IOCTL_INFER_NEW 설계: `memory/project_infer_new_irq_only.md`
- libedgetpu 대응 함수: `IsAllRunStatusKIdle` (run-status 게이트 동일 패턴)
