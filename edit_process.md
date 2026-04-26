# NPU Driver IOCTL_INFER Timeout 수정 과정

## 개요
Windows KMDF 커널 드라이버에서 Coral M.2 Edge TPU (Apex 칩)의 IOCTL_INFER 타임아웃 문제를 단계적으로 해결한 과정.

초기 증상: Inference 시작 후 1초 타임아웃, SCALAR_RUN_STATUS=0 (실행 안 됨)

---

## 1단계: 문제 분석 (진단 로그 추가)

**파일:** `npu_driver/Queue.c`

### 배경
IOCTL_INFER이 항상 타임아웃되었음. 원인을 파악하기 위해 충분한 진단 정보 필요.

### 수정 내용
- 페이지 테이블 엔트리(PTE) 확인: PTE[0], PTE[49], TRANSLATION_ENABLE 읽기
- 명령어 큐 설정 확인: QUEUE_BASE, QUEUE_TAIL, BREAKPOINT 읽기
- Scalar core 상태 확인: SCALAR_RUN_CONTROL, SCALAR_RUN_STATUS 읽기
- 타임아웃 시 상세 진단: WIRE_INT_PENDING, 각 모듈의 RUN_STATUS 등

### 결과
```
[DIAG] PTE[0]=0x... PTE[49]=0x... TRANS_EN=0x1
[DIAG] QUEUE_BASE=0x0 QUEUE_TAIL=0x30c60 BREAKPT=0x7fff
[POST-RUNCTL] SCALAR_RUN_CONTROL=0x0 STATUS=0x0  ← 문제 발견!
[TIMEOUT] WIRE_INT_PENDING=0x0, 모든 RUN_STATUS=0x0
```

핵심 발견: SCALAR_RUN_CONTROL이 0x0 → Scalar core가 실행되지 않음

---

## 2단계: MMU 번역 활성화

**파일:** `npu_driver/Queue.c`, IOCTL_INFER 처리 부분

### 배경
입출력 버퍼의 페이지 테이블 엔트리(PTE)를 쓰고 있었지만, MMU 번역이 활성화되지 않으면 PTE가 사용되지 않음.

IOCTL_MAP_BUFFER에서는 번역을 활성화했지만, IOCTL_INFER에서는 안 함.

### 수정 내용
```c
// 입출력 PTE 작성 후
apex_write_register(bar2, APEX_REG_TRANSLATION_ENABLE, 1);
DbgPrint("[%s] MMU translation enabled\n", __FUNCTION__);
```

위치: 페이지 테이블 PTE 쓰기 완료 후, 명령어 큐 설정 전

### 이유
- 방어적 프로그래밍: IOCTL_MAP_BUFFER를 호출하지 않은 경우에도 작동하도록
- 명시적: 모든 필요한 초기화를 한 곳에서 수행

---

## 3단계: Run Control 순서 및 TILE_CONFIG0 POLL

**파일:** `npu_driver/Queue.c`, run control 시작 부분

### 배경
libedgetpu (Linux 드라이버)의 run_controller.cc 분석 결과:

1. Scalar/Infeed/Outfeed/Parameter RUN_CONTROL을 먼저 쓰고
2. TILE_CONFIG0을 쓴 후
3. **TILE_CONFIG0이 실제로 쓰여질 때까지 POLL**하고
4. TILE RUN_CONTROL을 씀

하드웨어 주석: "Wait until tileconfig0 is set correctly. Subsequent writes are going to tiles, but hardware does not guarantee correct ordering with previous write."

### 수정 내용 (Before)
```c
apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);  // 먼저 설정
apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);  // 그 다음
apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);
// ... 나머지 RUN_CONTROL ...
```

### 수정 내용 (After)
```c
// 1. Scalar core와 데이터 경로 먼저 실행
apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);
apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);
apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1);
apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL, 1);

// 2. TILE_CONFIG0 설정 및 확인
apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
for (int retry = 0; retry < 100; retry++) {
    UINT64 tileConfigVal = apex_read_register(bar2, APEX_REG_TILE_CONFIG0);
    if (tileConfigVal == 0x7F) {
        DbgPrint("[%s] TILE_CONFIG0 verified (0x%llx)\n", __FUNCTION__, tileConfigVal);
        break;
    }
    KeStallExecutionProcessor(10);
}

// 3. TILE RUN_CONTROL들 (순서 보장된 후)
apex_write_register(bar2, APEX_REG_TILE_OP_RUN_CONTROL, 1);
apex_write_register(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL, 1);
apex_write_register(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL, 1);
```

### 이유
- 하드웨어의 순서 보장 요구
- TILE_CONFIG0 POLL = synchronization point

---

## 4단계: Reset 완료 확인 (SCALAR_RUN_CONTROL POLL)

**파일:** `npu_driver/Device.c`, npudriverEvtDevicePrepareHardware()

### 배경
libedgetpu의 QuitReset() 함수 분석:

```c
// 3. Confirm that moved out of reset by reading any CSR with known initial
// value. scalar core run control should be zero.
RETURN_IF_ERROR(
    registers_->Poll(scalar_core_offsets_.scalarCoreRunControl, 0));
```

Reset이 완전히 해제되었는지 확인하는 방법: SCALAR_RUN_CONTROL = 0 확인

### 수정 내용 (Device.c)
```c
// Quit-reset 후
DbgPrint("[%s] Starting GCB quit-reset sequence\n", __FUNCTION__);
// ... GCB reset 시퀀스 ...

// Critical: Confirm reset is completely released
for (int retry = 0; retry < 100; retry++) {
    UINT64 scRunCtrl = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_SCALAR_RUN_CONTROL);
    if (scRunCtrl == 0) {
        DbgPrint("[%s] Reset confirmed - SCALAR_RUN_CONTROL = 0x0\n", __FUNCTION__);
        break;
    }
    KeStallExecutionProcessor(100);
}
```

### 이유
- Reset이 완전히 해제되어야 CSR(Control Status Register) 접근이 유효함
- Reset 중에는 레지스터 쓰기가 작동하지 않음

---

## 5단계: Reset 타임아웃 감지 (로깅 추가)

**파일:** `npu_driver/Device.c`, GCB reset/quit-reset 대기 루프

### 배경
초기 코드: 대기 루프가 타임아웃되면 조용히 넘어감 → 문제 원인 파악 어려움

### 수정 내용
```c
// RAM shutdown 대기
int retry;
for (retry = 0; retry < 100; retry++) {
    UINT32 scu3_val = apex_read_register_32(..., APEX_REG_SCU_3);
    if ((scu3_val & (1 << 6)) != 0) {
        DbgPrint("[%s] RAM shutdown confirmed (SCU_3=0x%08x)\n", __FUNCTION__, scu3_val);
        break;
    }
    KeStallExecutionProcessor(100);
}
if (retry >= 100) {
    DbgPrint("[%s] WARNING: RAM shutdown timeout after 10ms (SCU_3=0x%08x)\n", 
             __FUNCTION__, scu3_val);
}

// RAM enable 대기도 유사하게
```

### 이유
- 숨겨진 문제 가시화
- 디버깅 효율성 향상

---

## 6단계: 레지스터 Width 수정 (64비트 → 32비트)

**파일:** `npu_driver/Queue.c`, run control 쓰기

### 배경
문제: 모든 수정에도 불구하고 SCALAR_RUN_CONTROL 쓰기가 작동하지 않음

분석:
- libedgetpu에서 SCU/Scalar core 레지스터를 **32비트** (Write32/Read32)로 접근
- 우리는 64비트 (Write/Read)로 접근

### 수정 내용 (Before)
```c
apex_write_register(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);        // 64비트
apex_write_register(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);        // 64비트
apex_write_register(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1); // 64비트
apex_write_register(bar2, APEX_REG_OUTFEED_RUN_CONTROL, 1);       // 64비트

UINT64 scRunCtrl = apex_read_register(bar2, APEX_REG_SCALAR_RUN_CONTROL);  // 64비트
```

### 수정 내용 (After)
```c
apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL, 1);        // 32비트
apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL, 1);        // 32비트
apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1); // 32비트
apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL, 1);       // 32비트

UINT32 scRunCtrl = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL);  // 32비트
```

### 이유
- 하드웨어 레지스터의 실제 width가 32비트
- 64비트 쓰기는 주변 레지스터도 영향을 미칠 수 있음

---

## 핵심 교훈

### 1. Reference Code 활용
Linux libedgetpu 소스 분석이 최대 수확:
- 정확한 레지스터 오프셋 검증
- 초기화 순서 확인
- Register width 발견

### 2. 진단 로그의 중요성
충분한 로그 없이는 문제 원인을 파악할 수 없음:
- 쓰기 성공 여부 확인
- 타임아웃 감지
- 각 단계의 상태 확인

### 3. 하드웨어 순서 보장
- TILE_CONFIG0 POLL: synchronization point 역할
- 하드웨어가 명시적으로 요구하는 순서 준수

### 4. Register Width 주의
- 일부 레지스터는 32비트, 일부는 64비트
- Reference code 확인 필수

### 5. 단계적 접근
- 한 번에 하나씩 수정
- 각 수정 후 테스트 결과 기록
- 거듭제곱식으로 최종 해결에 접근

---

## 수정 전후 비교

### 수정 전 (초기 상태)
```
[POST-RUNCTL] SCALAR_RUN_CONTROL=0x0 STATUS=0x0  ← 실행 안 됨
[TIMEOUT] WIRE_INT_PENDING=0x0
[TIMEOUT] SCALAR_RUN_STATUS=0x0
→ 1초 타임아웃
```

### 수정 후 (예상)
```
[npudriverEvtDevicePrepareHardware] Reset confirmed - SCALAR_RUN_CONTROL = 0x0
[DIAG] TILE_CONFIG0 verified (0x7f)
[POST-RUNCTL] SCALAR_RUN_CONTROL=0x1 STATUS=0x1  ← 실행 중!
[ISR] Interrupt pending: 0x...
[DPC] InferCompleteEvent set
→ IOCTL_INFER succeeded
```

---

## 파일 수정 요약

| 파일 | 수정 사항 | 줄 수 |
|------|---------|-------|
| Queue.c | MMU 번역 활성화 | +2 |
| Queue.c | Run control 순서 + POLL | ~30 |
| Queue.c | 32비트 접근 | -4/+4 |
| Device.c | Reset 타임아웃 감지 | ~20 |
| Device.c | SCALAR_RUN_CONTROL 확인 | ~15 |
| **합계** | | ~65 |

---

## 다음 단계 (예상)

1. 드라이버 재빌드
2. npu_test_console 실행
3. 다음 진단 결과 수집:
   - `[POST-RUNCTL] SCALAR_RUN_CONTROL=0x1` 확인
   - ISR 인터럽트 발생 확인
   - DPC에서 InferCompleteEvent 설정 확인
   - 최종적으로 IOCTL_INFER succeeded 확인

