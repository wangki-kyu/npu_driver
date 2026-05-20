# NPU Driver — 시스템 구조 & 동작 시퀀스

App ↔ Driver ↔ TPU 칩의 관계, 어떤 레지스터가 언제 켜지고 무슨 일이 일어나는지 정리한 문서.

---

## 1. 시스템 구조 (3계층)

```
┌─────────────────────────────────────────────────────────────────────────┐
│                                                                         │
│   APPLICATION (npu_test_console.exe)                  [User mode]       │
│   - bitstream 파일을 읽어 메모리에 로드                                 │
│   - 입력 이미지를 RAM에 준비                                            │
│   - 출력 버퍼를 RAM에 미리 0xCC로 채워둠                                │
│   - DeviceIoControl(...IOCTL_xxx...)로 드라이버에 명령 전달             │
│                                                                         │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ DeviceIoControl (Win32 API)
                             ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                                                                         │
│   DRIVER (npu_driver.sys, KMDF)                       [Kernel mode]     │
│                                                                         │
│   ┌─────────────────┐  ┌─────────────────┐  ┌──────────────────────┐    │
│   │ Device.c        │  │ Memory.c        │  │ Queue.c              │    │
│   │ - PrepareHW     │  │ - PageTableMap  │  │ - IOCTL dispatch     │    │
│   │ - ISR/DPC       │  │ - PTE write/rd  │  │ - INFER 제출 로직    │    │
│   │ - 인터럽트 라우팅│  │ - MDL lock      │  │ - 50ms 타임아웃 대기 │    │
│   └─────────────────┘  └─────────────────┘  └──────────────────────┘    │
│                                                                         │
│   ┌──────────────────────────────────────────────────────────────────┐  │
│   │ BAR0/BAR2 MMIO 매핑 (ioremap 격) — 칩 레지스터 세상으로 가는 창 │  │
│   │ BAR2: 0xa0000000 ~ +0x100000  (CSR + MSI-X 테이블 + Page Table) │  │
│   │ BAR0: 0xa0100000 ~ +0x4000    (보조)                            │  │
│   └──────────────────────────────────────────────────────────────────┘  │
│                                                                         │
└─────────────────────────────┬───────────────────────────────────────────┘
                              │ PCIe (config + memory + MSI-X)
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                                                                         │
│   TPU 칩 (Apex/Beagle, PCI 0x1ac1:0x089a)             [Silicon]         │
│                                                                         │
│   ┌─────────────────────────────────────────────────────────────────┐   │
│   │                       HIB (Host Interface Block)                │   │
│   │  - 페이지테이블 워커: device VA → host PA 변환                  │   │
│   │  - DMA: AXI master ↔ PCIe outbound TLP                          │   │
│   │  - INSTR_QUEUE 디스크립터 fetch                                 │   │
│   └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│   ┌─────────────────────────────────────────────────────────────────┐   │
│   │                       GCB (Gemini Compute Block)                │   │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐           │   │
│   │  │ SCALAR   │  │ INFEED   │  │ OUTFEED  │  │ AVDATA │           │   │
│   │  │ (CPU 격) │  │ (input)  │  │ (output) │  │        │           │   │
│   │  └──────────┘  └──────────┘  └──────────┘  └────────┘           │   │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐                       │   │
│   │  │ TILE×N   │  │ MESH     │  │ RING BUS │                       │   │
│   │  │ (matmul) │  │          │  │          │                       │   │
│   │  └──────────┘  └──────────┘  └──────────┘                       │   │
│   └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│   ┌─────────────────────────────────────────────────────────────────┐   │
│   │  CSR + MSI-X 테이블 (BAR2 안에서 register-mapped)               │   │
│   │  드라이버가 read/write로 칩과 대화하는 "공용 게시판"            │   │
│   └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 시간순 시퀀스

### Phase 1 — 드라이버 로드 (`DriverEntry` → `DeviceAdd` → `PrepareHardware`)

```
[App 아직 안 띄워짐]

OS:                            Driver:                     Chip:
──────────                     ──────────────────────      ────────────────
                              DriverEntry()
PnP가 디바이스 발견           ─────────────────────►
                              EvtDeviceAdd()
                                ├ WDF context 할당
                                ├ WdfInterruptCreate × 4   (MSI-X 통 4개 만듦)
                                │   각 통에 ISR/DPC 콜백 부착
                                └ 디바이스 인터페이스 생성
                                                            (아직 IRQ 안 옴)

                              EvtDevicePrepareHardware()
                                ├ BAR0/BAR2 ioremap
                                │
                                ├ [STEP 1] MSI-X SAVE
                                │   BAR2+0x46800 에서 4벡터 dump
                                │   ▼ 칩이 host APIC addr/data 미리 가지고 있음
                                │   (벡터별 0xfee3f00c / 0x49b1...)
                                │   → SavedMsixTable[]에 백업
                                │
                                ├ [STEP 2] PageTable 초기화
                                │   8192 entry × 8B = 64KB host RAM 할당
                                │   PA를 PAGE_TABLE 레지스터에 등록
                                │
                                ├ [STEP 3] SCU/PHY 워밍업
                                │   SCU_CTRL_0 = PHY 인액티브 모드 클리어
                                │   IDLEGENERATOR = 0x1
                                │   DMA pause/unpause 사이클
                                │
                                ├ [STEP 4] GCB 리셋
                                │   - RAM shutdown (SCU_3 비트들)
                                │   - gcbb_credit0 펄스 (BULK 크레딧 클리어)
                                │   - RAM enable
                                │   - reset_done 폴링
                                │   ▼ GCB 내부 SRAM 다 비워짐 (MSI-X 포함)
                                │
                                ├ [STEP 5] AXI quiesce 해제
                                │   SCU_2 [19:18] = 0 → AXI_QUIESCE bit16=0
                                │   SCU_2 [19:18] = 2 (force on)
                                │
                                ├ [STEP 6] 클럭/MMU 재프로그램
                                │   PAGE_TABLE_SIZE = 8192
                                │   EXTENDED_TABLE = 6144
                                │   KERNEL_HIB_TRANSLATION_EN = 1
                                │
                                ├ [STEP 7] 인터럽트 라우팅 ★ 핵심
                                │   SC_HOST_INTVECCTL    = 0  (vec0)
                                │   SC_HOST_INT_CONTROL  = 0xF (4채널 켬)
                                │   TOP_LEVEL_INT_CONTROL= 0xF
                                │   FATAL_ERR_INT_CONTROL= 1
                                │   INSTR_QUEUE_INTVECCTL= 0
                                │   INPUT/PARAM/OUTPUT_ACTV/FATAL_ERR INTVECCTL = 0
                                │   INSTR_QUEUE_INT_CONTROL = 1
                                │   TOP_LEVEL_INTVECCTL  = 0
                                │   WIRE_INT_MASK        = 0  (전부 unmask)
                                │
                                ├ [STEP 8] MSI-X RESTORE ★ 핵심
                                │   SavedMsixTable[] → BAR2+0x46800
                                │   ▼ 리셋으로 사라진 host APIC addr 복구
                                │
                                ├ [STEP 9] DescRing/StatusBlock 등록
                                │   PTE[6142] = DescRing 호스트 PA
                                │   PTE[6143] = StatusBlock 호스트 PA
                                │   INSTR_QUEUE_BASE      = 0x17fe000 (device VA)
                                │   STATUS_BLOCK_BASE     = 0x17ff000
                                │   INSTR_QUEUE_SIZE      = 256
                                │   INSTR_QUEUE_DESC_SIZE = 16
                                │   INSTR_QUEUE_TAIL      = 0
                                │   INSTR_QUEUE_CONTROL   = 0x1 (enable)
                                │   INSTR_QUEUE_CONTROL   = 0x5 (+sb_wr_enable)
                                │
                                ├ [STEP 10] BREAKPOINT은 손대지 않음
                                │   POR default(0x0) 그대로 → 메모리 invariant
                                │
                                ├ [STEP 11] 모든 엔진 RUN_CONTROL=1
                                │   SCALAR / INFEED / OUTFEED / AVDATA / PARAM_POP
                                │   TILE_OP / NARROW_TO_WIDE / WIDE_TO_NARROW
                                │   MESH0 / RING_BUS_PRODUCER / CONSUMER0/1
                                │   ▼ 엔진들 kRunning idle-loop
                                │
                                └ Exit OK

                              EvtInterruptEnable × 4
                                ├ MessageSignaled=1 확인
                                ├ INT_VECCTL/MASK readback dump
                                └ MSI-X 테이블 readback dump
                                                            ▼ 이 시점부터
                                                            칩이 MSI write TLP
                                                            발사 가능 상태
```

### Phase 2 — Application 부팅 후 첫 IOCTL (PARAM 등록)

```
App                           Driver                        Chip
──────                        ─────────────────────         ──────────
fopen("model.bin")
fread(...)
VirtualAlloc(...)

DeviceIoControl(0x22a004,
  MAP_BUFFER {
    UserAddr=...,
    Size=0x2650,
    ReqDeviceVA=0x0
  })          ──────────────► IOCTL_MAP_BUFFER
                              ApexPageTableMap()
                                ├ MmProbeAndLockPages → MDL 잡음 (호스트 RAM 페이지 핀)
                                ├ 페이지별 PA 추출
                                └ 각 PTE = (PA | valid bit)
                                  ▼ APEX_REG_PAGE_TABLE + idx*8 에 write
                                                            ▼ 칩 내부 PageTable RAM 에 기록
                                                            (이후 device VA로 host RAM 접근 가능)

DeviceIoControl(0x22a011,
  PARAM_CACHE {bitstream})  ──► IOCTL_PARAM_CACHE
                              ├ 1500 페이지 lock (param data)
                              ├ PTE[3..1502] write (device VA 0x3000~)
                              ├ "MAPPING-ONLY" — 칩에 안 보냄
                              └ PARAM 데이터는 칩 입장에선 그냥 host RAM에 있음
```

### Phase 3 — INFER 제출 (가장 중요)

```
App                           Driver                              Chip
──────                        ──────────────────────              ──────────
input/output/bitstream
모두 RAM에 준비

DeviceIoControl(0x22a004,
  MAP_BUFFER {INFER bitstream,
   ReqDeviceVA=0x600000})   ──► PTE[1536..1584] write

DeviceIoControl(0x22a015,
  IOCTL_INFER {input/output})
                              ├ 입력 PTE[1585..]    write
                              ├ 출력 PTE[1660..1663] write (kVA에 0xCC pre-fill)
                              │
                              ├ DescRing[slot 0] = PARAM bitstream descriptor
                              │   {device VA=0x0, size=0x2650}
                              ├ DescRing[slot 1] = INFER bitstream descriptor
                              │   {device VA=0x600000, size=0x30c60}
                              │
                              ├ INSTR_QUEUE_TAIL ← 2  ★ 칩에 "submit" 신호
                              │                                   ▼
                              │                                   IQ가 DescRing[0..1] fetch
                              │                                   ▼
                              │                                   PARAM bitstream 디코드
                              │                                   → SCALAR가 PARAM_CACHE
                              │                                     명령들 실행
                              │                                     (param data를
                              │                                      host RAM→GCB 내부
                              │                                      tile RAM으로 미리 적재)
                              │                                   ▼
                              │                                   IQ COMPLETED ← 1
                              │                                   IQ_INT_STATUS bit0 set
                              │                                   WIRE_INT_PENDING bit0 set
                              │                                   ▼
                              │                                   ★ MSI-X TLP 발사
                              │                                     (data=0x49b1, addr=APIC)
                              │                                   ▼
                              │   [ISR#1] 호출 ◄─────────── (CPU APIC → Windows ISR)
                              │     pending=0x11 iq_int=0x1
                              │     ack: IQ_INT_STATUS=0x1 W1C
                              │           WIRE_INT_PENDING=0x11 W1C
                              │     QueueDpcForIsr
                              │   [DPC]
                              │     SCALAR=0x1 OUT=0x0
                              │     → "PREMATURE" deferring
                              │     unlock 안 함, signal 안 함
                              │
                              │                                   ▼
                              │                                   INFER bitstream 디코드
                              │                                   → INFEED가 input image
                              │                                     host RAM → tile RAM
                              │                                   → TILE/MESH 연산 (≈6ms)
                              │                                   → OUTFEED가 output을
                              │                                     tile RAM → device VA로
                              │                                     (여기까지는 정상)
                              │                                   → device VA→host PA 변환
                              │                                     ▼
                              │                                     ★ outbound TLP 발사 시도
                              │                                     ↑↑↑ 현재 막혀있는 곳
                              │                                     (PCIe Device Status=0)
                              │                                   ▼
                              │                                   SCALAR가 host_interrupt
                              │                                   명령 실행
                              │                                   → SC_HOST bit set
                              │                                   → WIRE_INT_PENDING |= 0x10
                              │                                   ▼
                              │                                   ★ MSI-X TLP 두 번째 발사
                              │   [ISR#2] 호출 (실제 로그엔 이게 안 옴 — bit 매핑 의문)
                              │
                              │ (인터럽트가 안 와도 폴링이 살림)
                              ├ 폴링 루프:
                              │   COMPLETED == TAIL 되면 완료 처리
                              │   ▼
                              │   [50MS] inference truly done
                              │
                              ├ output kVA 읽기
                              │   → 0xCC 그대로 (★ 본 문제)
                              │
                              └ STATUS_SUCCESS 리턴

App: 출력 검사 → 전부 0xCC ▲▲▲ 현재 디버깅 중
```

### Phase 4 — 정리

```
DeviceIoControl(UNMAP)      ─► PTE[1536..1584] = 0
                              MmUnlockPages, IoFreeMdl

App 종료                    ─► EvtFileCleanup
                                cached PARAM/bitstream MDL 해제
```

---

## 3. 핵심 레지스터 역할 한 페이지 정리

| 레지스터 | 역할 | 언제 건드리나 |
|---|---|---|
| `PAGE_TABLE` (BAR2+0x...) | device VA → host PA 변환 테이블 | MAP_BUFFER마다 |
| `PAGE_TABLE_SIZE` / `EXTENDED_TABLE` | 변환 테이블 크기/분할 | PrepareHW 1회 |
| `KERNEL_HIB_TRANSLATION_EN` | MMU on/off | PrepareHW 1회 |
| `INSTR_QUEUE_BASE/SIZE/TAIL/CONTROL` | descriptor ring 설정 | PrepareHW 1회 + 매 INFER에 TAIL |
| `STATUS_BLOCK_BASE` | 칩이 진행 상황 적는 host RAM 주소 | PrepareHW 1회 |
| `*_RUN_CONTROL` (각 엔진) | 엔진 enable | PrepareHW 1회 |
| `*_RUN_STATUS` (각 엔진) | 엔진 현재 상태 (0=Idle, 1=Run, 4=Halted) | 폴링 read |
| `WIRE_INT_PENDING` | 모든 인터럽트 소스의 OR (집계) | ISR이 W1C ack |
| `WIRE_INT_MASK` | 마스크 | PrepareHW에서 0 |
| `IQ_INT_STATUS` | "descriptor fetch 끝" 비트 | 칩이 set, ISR이 W1C |
| `SC_HOST_INT_CONTROL` | SCALAR→host 채널 4개 enable | PrepareHW에서 0xF |
| `*_INTVECCTL` | 어느 소스를 어느 MSI-X 메시지로 보낼지 | PrepareHW에서 모두 0 |
| `MSI-X TABLE` (BAR2+0x46800) | 칩이 MSI 발사할 host APIC addr/data | OS가 enum 시 채움, GCB 리셋 후 우리가 RESTORE |
| `USER_HIB_ERROR_STATUS` | 페이지폴트 등 에러 비트 | 항상 read |
| `AXI_W/AW_CREDIT_*` | 칩 *내부* AXI 통계 (outbound PCIe와 무관) | 진단 read |
| PCI config Device Status | 칩 *outbound* TLP 결과 (UnsupReq 등) | 진단 read |

---

## 4. "지금 어디까지 작동하는가" 한 줄 지도

```
App ─── IOCTL ─── Driver ─── BAR write ─── 칩 CSR ─── PTE programming ─── ✅
                                                                          ▼
                            ─── DescRing fetch (chip-side) ──── ✅
                                                                          ▼
                            ─── SCALAR + INFEED + TILE 연산 ─── ✅
                                                                          ▼
                            ─── OUTFEED tile→AXI master ────── ✅ (axi_w_insertion 증가)
                                                                          ▼
                            ─── AXI master → PCIe outbound TLP ❌ 막혀있음
                                                                          ▼
                            ─── host RAM에 결과 write ─────── ❌ 0xCC 그대로
                                                                          ▼
                            ─── SC_HOST 인터럽트 발사 ──── ⚠️ 비트 매핑 의문 (0x1000 vs 0x10)
                                                                          ▼
                            ─── 드라이버 ISR ack ────────── ✅ (회귀 회복)
                                                                          ▼
                            ─── App에 STATUS_SUCCESS ──── ✅ (단, 데이터 없음)
```

---

## 5. 확대도 — GCB 리셋 시퀀스

`PrepareHardware`의 STEP 4 박스 하나를 풀어서 본 것. 이 시퀀스의 의도는 **"칩 내부 SRAM과 클럭/DMA 상태를 깨끗한 0에서 다시 켜기"**. libedgetpu `beagle_top_level_handler.cc` 의 `EnableReset` + `QuitReset` + `DisableSoftwareClockGate` + `DisableHardwareClockGate`를 묶은 것.

```
                                            ┌─────────────────────────────┐
                                            │   리셋 진입(EnableReset)    │
                                            └─────────────────────────────┘
[A] SCU_2 bit2  ← 1   (RMW)
    └ "GCB 리셋 enable"
[B] SCU_2 bit18 ← 1   (RMW)
    └ "rg_gated_gcb = HW clock gated"  (게이팅 켜고 들어가야 안전)
[C] SCU_3 bit14 ← 1   (RMW)  — 실제로는 [15:14]=0x3
    └ "RAM shutdown 강제"
                                            ▼
                            ┌──────────────────────────────────┐
                            │ POLL: SCU_3 bit6 == 1 (RAM down) │  최대 10ms (100us×100)
                            │ → "RAM shutdown confirmed"       │
                            └──────────────────────────────────┘
                                            ▼
[D] GCBB_CREDIT0 ← 0xF                ┌────────────────────────────┐
[E] GCBB_CREDIT0 ← 0x0                │   stale BULK 크레딧 펄스   │
                                      │   (없으면 다음 세션 INFEED │
                                      │    kHalted=4로 멈춤)       │
                                      └────────────────────────────┘
                                            ▼
                                            ┌─────────────────────────────┐
                                            │  리셋 해제(QuitReset)       │
                                            └─────────────────────────────┘
[F] SCU_3 bit14 ← 0   (RMW)           "RAM shutdown 해제"
[G] SCU_2 bit18 ← 2   (RMW)           "rg_gated_gcb = 클럭 강제 ON"
[H] SCU_2 bit2  ← 2   (RMW)           "리셋 EXIT"
                                            ▼
                            ┌──────────────────────────────────┐
                            │ POLL: SCU_3 bit6 == 0 (RAM up)   │  최대 10ms
                            │ → "RAM enable confirmed"         │
                            └──────────────────────────────────┘
                                            ▼
                            ┌──────────────────────────────────┐
                            │ POLL: SCALAR_RUN_CONTROL == 0    │  최대 10ms
                            │ → "Reset confirmed"              │
                            └──────────────────────────────────┘
                                            ▼
                                            ┌─────────────────────────────┐
                                            │  파워/클럭 후처리           │
                                            └─────────────────────────────┘
[I] SCU_3 [27:26] ← 0x3              "rg_pwr_state_ovr = all-modes-off"
                                     (Inactive/Sleep 절대 못 들어가게)

[J] SCU_2 [19:18] ← 0                "DisableSoftwareClockGate Step A1"
[K] AXI_QUIESCE bit16 ← 0            "AXI quiesce 요청 해제"
                                            ▼
[L] SCU_2 [19:18] ← 2                "DisableHardwareClockGate Step B"
                                            ▼
                            ┌──────────────────────────────────┐
                            │ POLL: AXI_QUIESCE bit21 == 0     │  최대 10ms (10us×1000)
                            │ → "axi_quiesced 상태 떨어짐"     │
                            └──────────────────────────────────┘
                                            ▼
[M] USER_HIB_DMA_PAUSE ← 0           "DMA unpause"
                                            ▼
                                          (완료)
```

**왜 이 순서인가**

- `SCU_2 bit18`을 먼저 1(gated)로 했다가 나중에 2(force-on)로 바꾸는 두 단계: 리셋 도중에는 게이팅 켠 채로 SRAM을 끄는 게 안전. 곧바로 force-on 하면 RAM의 axi_quiesced 비트가 안 풀림.
- `gcbb_credit0` 펄스 (D-E): GCB↔HIB 사이 AXI 브리지가 직전 세션의 BULK 크레딧을 latch한 상태로 남아 있으면 새 INFEED 호출이 내부에서 hang → INFEED auto-halt(`RUN_STATUS=4`). 메모리 파일에도 적힌 함정.
- `SCU_3 [27:26]=0x3` (I): low-power state로 떨어지면 RUN_CONTROL write가 silent fail 됨. Edge TPU 같은 디바이스는 power-state polarity가 반전된 경우가 많아 `0x3=all-disabled`가 정답.
- 마지막 `DMA_PAUSE=0` (M): 이전에 일부러 pause 걸었던 걸 명시적으로 해제. 리셋이 이걸 자동으로 풀어준다는 보장이 없음.

**부수적 invariant (메모리 파일)**

- 리셋이 BAR2+0x46800 (MSI-X 테이블)을 0으로 밀어버림 → 별도 SAVE/RESTORE 필요.
- `*_BREAKPOINT` 레지스터는 POR default 그대로 둘 것 (0x7FFF로 쓰면 SCALAR가 mid-INFER halt).

---

## 6. 확대도 — PageTable 워커

`Memory.c`의 `ApexPageTableInit / Map / Unmap`에 대응. 칩이 device VA로 host RAM에 접근할 때 거치는 변환 경로.

### 6.1 데이터 구조

```
호스트 RAM (드라이버가 ExAllocatePoolWithTag로 할당한 64KB)
┌────────────────────────────────────────────────┐
│ APEX_PTE  PageTableBase[8192]                  │   (드라이버 측 섀도우 — 드라이버만 봄)
│   struct {                                     │
│     UINT64 address;       // 호스트 PA (40-bit)│
│     UINT32 status;        // FREE(0)/INUSE(1)  │
│     UINT32 dma_direction; //                   │
│   }                                            │
│ ※ 16바이트 × 8192 entries = 128KB              │
└────────────────────────────────────────────────┘
                  │
                  │ MmGetPhysicalAddress
                  ▼
              PageTablePhys ──────────────────────────────┐
                                                          │ "이 주소에서 PTE 읽어라"
                                                          ▼
칩 BAR2 안의 CSR (PCIe MMIO write로 직접 보임)
┌────────────────────────────────────────────────┐
│ APEX_REG_PAGE_TABLE       (오프셋 베이스)      │
│ APEX_REG_PAGE_TABLE_SIZE  = 8192               │
│ APEX_REG_EXTENDED_TABLE   = 6144 (*1)          │
│ APEX_REG_KERNEL_HIB_TRANSLATION_EN = 1         │
└────────────────────────────────────────────────┘
                  │
                  │ HIB의 PageTable Walker가 device VA를 PA로 변환
                  ▼
실 호스트 RAM 페이지 (MmProbeAndLockPages로 핀된 user buffer)

(*1) PTE[0..6143]은 "simple" (직접 4KB), PTE[6144..8191]은 "extended" (2-level).
```

### 6.2 Map 시퀀스 (`ApexPageTableMap`)

```
입력: UserAddr=0x172e6dc3000, Size=0x2650, *DeviceAddress=0x0
                                                                       
[1] startPte = DeviceAddress >> 12        = 0
    pageCount = ceil(Size / 4096)         = 3        (0x2650 / 4096 = 2.59 → 3)
                                                                       
[2] IoAllocateMdl(UserAddr, Size)         → mdl       (호스트 가상주소 추적)
[3] MmProbeAndLockPages(mdl, IoWriteAccess)
    ▲ 페이지 핀: 이후 OS가 swap-out 못 함, PA 안정화
                                                                       
[4] PFN[] = MmGetMdlPfnArray(mdl)         (페이지 프레임 번호 배열)
                                                                       
[5] WdfSpinLockAcquire(PageTableLock)
                                                                       
    for (i = 0..pageCount-1) {
       PA = PFN[i] << 12                                                
                                                                       
       // 드라이버 섀도우 갱신
       PageTableBase[startPte+i].address    = PA & 0x00FFFFFFFFFFFFFF
       PageTableBase[startPte+i].status     = INUSE                     
       PageTableBase[startPte+i].dma_direction = 0
                                                                       
       // 칩 CSR에 직접 write (★ 칩의 PTE RAM에 즉시 반영)
       apex_write_register(bar2,                                        
           APEX_REG_PAGE_TABLE + (startPte+i)*8,                        
           PA | 0x1)            ← bit0 = valid
    }
                                                                       
[6] WdfSpinLockRelease
[7] LockedModelMdl = mdl   (UNMAP/cleanup에서 unlock하기 위해 보관)
                                                                       
출력: *DeviceAddress = 0x0  (그대로, 드라이버가 정해준 device VA)
```

### 6.3 칩이 device VA를 사용하는 흐름 (런타임)

```
SCALAR/INFEED/OUTFEED 엔진이 device VA 0xABC123 접근 시도
          │
          ▼
HIB PageTable Walker
          │
          ├─ vaPage   = (0xABC123) >> 12  = 0xABC
          ├─ pteEntry = MMIO read at  PAGE_TABLE_BASE + 0xABC*8
          │   (실제로는 칩 내부 PTE RAM에 직접 들어 있음 — BAR2 write로 설정한 값)
          │
          ├─ pteEntry & 0x1 == 0  →  page fault → USER_HIB_ERROR_STATUS bit set
          │                          INFEED_PAGE_FAULT_ADDR / USER_HIB_FIRST_ERROR 기록
          │
          └─ pteEntry & 0x1 == 1  →  hostPA = (pteEntry & ~0xFFF) | (VA & 0xFFF)
                                     ▼
                                     PCIe outbound TLP 생성 (read 또는 write)
                                     주소 = hostPA
                                     ▼
                                     루트 컴플렉스 → 호스트 RAM
```

### 6.4 PTE 인덱스 사용 규약 (이 드라이버가 쓰는 device VA 영역)

| device VA 범위 | PTE 인덱스 | 용도 | 누가 |
|---|---|---|---|
| `0x000000` ~ `0x002FFF` | `[0..2]` | PARAM bitstream (3 페이지) | PARAM_CACHE 시 |
| `0x003000` ~ `0x5DAFFF` | `[3..1502]` | PARAM data (1500 페이지) | PARAM_CACHE 시 |
| `0x600000` ~ `0x630BFF` | `[1536..1584]` | INFER bitstream (49 페이지) | IOCTL_INFER 시 |
| `0x631000` ~ `0x67BFFF` | `[1585..1659]` | 입력 이미지 (75 페이지) | IOCTL_INFER 시 |
| `0x67C000` ~ `0x67FFFF` | `[1660..1663]` | 출력 버퍼 (4 페이지) | IOCTL_INFER 시 |
| `0x17FE000` | `[6142]` | DescRing | PrepareHardware 1회 |
| `0x17FF000` | `[6143]` | StatusBlock | PrepareHardware 1회 |

**왜 6142/6143 인가**: PAGE_TABLE_SIZE=8192이고 EXTENDED_TABLE=6144이므로 simple-PTE 영역(0..6143) 의 마지막 두 자리. libedgetpu/gasket이 동일 위치를 사용.

---

## 7. 확대도 — IOCTL_INFER 내부 (Descriptor Ring 제출)

`Queue.c`의 `IOCTL_INFER` / `IOCTL_INFER_WITH_PARAM` 핸들러를 풀어서 본 것. 가장 중요한 한 시퀀스는 **DescRing에 16바이트 디스크립터를 쓰고 INSTR_QUEUE_TAIL을 칩에 알리는 부분**.

### 7.1 자료구조

```
DescRing (드라이버가 PrepareHardware에서 4KB NonPagedPool로 할당)
host VA: DescRingBase  (커널)
host PA: 0x4392d5000 (예시) — PTE[6142]에 등록됨
device VA: 0x17FE000 (PrepareHardware에서 INSTR_QUEUE_BASE 레지스터에 쓴 값)

┌─────────────────────────────────────────────────────────────────────┐
│ HOST_QUEUE_DESC ring[256]                                           │
│   struct {                                                          │
│     UINT64 address;        // bitstream 시작 device VA              │
│     UINT32 size_in_bytes;  // bitstream 바이트 수                   │
│     UINT32 reserved;       // 0                                     │
│   }                                                                 │
│   ※ 16바이트 × 256 = 4096바이트 = 한 페이지                         │
└─────────────────────────────────────────────────────────────────────┘

DescRingTail  (드라이버 측 카운터 — 다음에 쓸 슬롯의 모듈로 전 값)
INSTR_QUEUE_TAIL  (칩 CSR — 드라이버가 write하면 칩이 fetch 시작)
INSTR_QUEUE_FETCHED_HEAD (칩 CSR — 칩이 어디까지 fetch 했는지)
INSTR_QUEUE_COMPLETED_HEAD (칩 CSR — 칩이 어디까지 실행 완료했는지)

StatusBlock (4KB, PTE[6143])
  StatusBlock[0] = COMPLETED_HEAD 의 거울 (칩이 outbound write로 갱신해줌)
```

### 7.2 IOCTL_INFER_WITH_PARAM 의 디스크립터 작성 ★

`Queue.c:563-582` (가장 중요한 30라인)

```
입력 (IOCTL_INFER_INFO):
  pInput->BitstreamDeviceVA  = 0x600000   (이번 INFER bitstream 의 device VA)
  pInput->BitstreamSize      = 0x30c60
  pInput->Input/Output       = (이미 별도 IOCTL_MAP_BUFFER 로 PTE 등록되어 있음)
캐시 (DEVICE_CONTEXT):
  CachedParamBitstreamDeviceVA = 0x0
  CachedParamBitstreamSize     = 0x2650
                                                                       
[1] slotP = DescRingTail % 256                                         
    slotI = (DescRingTail + 1) % 256                                   
                                                                       
[2] PARAM 디스크립터 작성                                              
    ring[slotP].address       = 0x0       ← PARAM bitstream device VA 
    ring[slotP].size_in_bytes = 0x2650                                 
    ring[slotP].reserved      = 0                                      
                                                                       
[3] INFER 디스크립터 작성                                              
    ring[slotI].address       = 0x600000  ← INFER bitstream device VA 
    ring[slotI].size_in_bytes = 0x30c60                                
    ring[slotI].reserved      = 0                                      
                                                                       
[4] KeMemoryBarrier()  ★ 중요                                         
    └ 드라이버의 ring 쓰기와 다음 TAIL 쓰기 사이의 store reorder 방지  
                                                                       
[5] DescRingTail += 2                                                  
                                                                       
[6] apex_write_register(bar2, INSTR_QUEUE_TAIL, DescRingTail)          
    │                                                                  
    │  (BAR2 MMIO write — PCIe configured-write TLP)                   
    ▼                                                                  
    [칩이 TAIL 변화 감지]                                              
    │                                                                  
    │ ── 칩이 outbound read TLP 발사 ───►  host PA 0x4392d5000 부터    
    │                                       16 × N 바이트 fetch        
    │ ◄── PCIe completion: ring[slotP], ring[slotI] 데이터 수령        
    │                                                                  
    │ INSTR_QUEUE_FETCHED_HEAD ← FETCHED+2                              
    │                                                                  
    │ 디스크립터 디코드 → 각 bitstream device VA 에서 instruction stream│
    │                     (PARAM_CACHE 명령들 / INFER 명령들) 페치     │
    │                                                                  
    │ SCALAR가 스트림 실행                                             │
    │  → param data 적재 (PARAM 디스크립터)                            │
    │  → INFEED/TILE/MESH/OUTFEED 연산 (INFER 디스크립터)              │
    │                                                                  │
    │ 각 디스크립터 끝나면:                                            │
    │  - INSTR_QUEUE_COMPLETED_HEAD 증가                               │
    │  - StatusBlock[0] 도 같이 갱신 (outbound write to PTE[6143])     │
    │  - IQ_INT_STATUS bit0 set                                        │
    │  - WIRE_INT_PENDING 집계 비트 set                                │
    │  - MSI-X TLP 발사  ──► Host ISR                                  │
    ▼                                                                  
[7] (드라이버 쪽) 폴링 루프 또는 ISR 대기                              
    [POLL@Nms] OUTFEED/INFEED RUN_STATUS read                          
    [50MS] 체크: COMPLETED_HEAD == TAIL ?                              
                                                                       
[8] 완료 처리: PTE 그대로 두고 (UNMAP에서 정리), 출력 kVA 검사,       
    STATUS_SUCCESS 리턴
```

### 7.3 IOCTL_INFER (단일 디스크립터) 와의 차이

`Queue.c:583-601`. PARAM_CACHE를 별도 IOCTL로 미리 등록만 해두고 INFER 호출 시 매번 PARAM 디스크립터를 같이 안 보내는 모드. 디스크립터 1개만 ring에 쓰고 TAIL+=1.

지금 디버그 흐름은 **항상 `INFER_WITH_PARAM`** 사용 — PARAM 디스크립터를 매 INFER마다 재submit (libedgetpu와 동일). 이유: 단일 INFER만 보내면 SCALAR가 PARAM 컨텍스트 없이 시작해서 INFEED가 페이지폴트 또는 즉시 halt.

### 7.4 RUN_STATUS 변화 (정상 INFER 한 사이클의 시간축)

```
시각      SCALAR  INFEED  OUTFEED  AVDATA  비고
------    ------  ------  -------  ------  --------------------------------------
TAIL write 0       0       0        0       (모두 kIdle, idle-loop)
+0..1ms    1       1       0        1       SCALAR가 PARAM 디스크립터 fetch 시작
                                            INFEED idle-loop 살아남
+1ms       1       1       0        1       PARAM_CACHE 진행, IQ COMPLETED=1
                                            ★ ISR#1 (pending=0x1, IQ_INT)
                                            DPC: SCALAR=1 → "PREMATURE", defer
+2..6ms    1       1       0        1       INFER 디스크립터 실행 중
                                            (TILE/MESH/RING 활동)
+7..8ms    1       1       1        1       OUTFEED kRun (★ axi_w_insertion 증가)
                                            OUTFEED writeback (정상이면 host RAM에)
+9ms       0       0       0        0       SCALAR가 host_interrupt 명령 실행
                                            ★ ISR#2 기대 (pending에 SC_HOST 비트)
+9~50ms    0       0       0        0       (폴링이 COMPLETED_HEAD == TAIL 확인)
```

### 7.5 디스크립터를 못 받으면 / 잘못 쓰면

| 증상 | 원인 |
|---|---|
| `FETCHED_HEAD` 가 TAIL 못 따라감 | DescRing host PA 가 PTE[6142]에 없음 / valid bit 미설정 |
| `USER_HIB_ERROR bit5` (instr_queue_bad_configuration) | DESC_SIZE 잘못 (이 칩은 16) |
| `USER_HIB_ERROR bit9` (instr_queue_invalid) | INSTR_QUEUE_CONTROL 안 켬 |
| 디스크립터는 fetch되지만 SCALAR 진행 X | bitstream device VA의 PTE 미설정 또는 valid 미세트 |
| `INFEED_RUN_STATUS=4 (kHalted)` | gcbb_credit0 펄스 누락 (메모리 파일) — GCB 리셋 시퀀스의 [D]-[E] 단계 |

이 표가 디버깅 시 출발점. 어떤 비트가 떴느냐로 어느 단계에서 막혔는지 역추적.

