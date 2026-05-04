# KMDF test_console 가이드 — Edge TPU outfeed 검증

KMDF 커스텀 드라이버에서 직접 칩에 infeed/outfeed 를 태우고, byte-단위 golden 비교로 동작을 검증하기 위한 종합 노트. `run_infer.py` (실제 SSD 얼굴 추론) 와 `run_add_infer.py` (add_int8 단순 검증) 로 사전 검증한 내용을 기반으로 한다.

---

## 1. 검증 모델 두 개의 역할

| 단계 | 파일 | 그래프 | 검증 범위 |
|---|---|---|---|
| 1 | `model/identity_int8.tflite` (+ edgetpu 컴파일본) | `MAXIMUM(x, x)` ≈ `y = x` | **데이터 경로만**: DMA, infeed/outfeed, 버퍼 정렬, 메모리 매핑, cache coherency |
| 2 | `model/add_int8.tflite` (+ edgetpu 컴파일본) | `ADD(x, 1.0)` (`y = x + 1`) | **데이터 경로 + 실제 산술 + requantize**: ADD ALU, 상수 텐서 로딩, input/output scale·zp 재정렬, saturation |

순서: **identity 통과 후 add 진행**. identity 가 깨지면 산술 검증할 의미가 없음 (DMA/매핑부터 의심).

### 1.1 왜 단순한 모델인가
- 신경망이 아니다. weight 없음. ADD op 1개 + dequant/quant 2개가 그래프 전부.
- "단순 = 약한 검증" 이 아니다. 칩이 통과하려면:
  1. infeed 로 16바이트 수신 (DMA)
  2. int8 → 내부 표현 dequantize
  3. 상수 텐서 `1.0` 정확히 로딩 후 ADD
  4. **다른** scale/zp 로 requantize
  5. `[-128, 127]` saturation
  6. outfeed 로 16바이트 송신 (DMA)
- 실패 지점이 명확해서 디버깅 좁히기 쉬움.

### 1.2 왜 add 의 byte 가 input 과 같은가
입력 `f8f9...0607` 을 add 모델에 넣으면 출력도 `f8f9...0607` 로 나오지만 **의미가 다름**.

| | byte | scale | zero_point | real value |
|---|---|---|---|---|
| input  `0xf8` | -8 | 0.007837736 | 0    | -0.0627 |
| output `0xf8` | -8 | 0.007839226 | -128 | **+0.9405** |

같은 byte 인데 한쪽은 음수, 한쪽은 +1 근처의 양수. **단순 byte 복사로는 절대 이 패턴이 안 나온다.** 칩이 정말로 +1 을 더하고 zp 를 -128 로 시프트해서 quantize 해야만 나오는 결과 — 그래서 강력한 검증.

---

## 2. Golden vector

`model/golden.json` 의 `add_int8`/`identity_int8` 섹션에 input/output 16바이트 벡터와 quantization 파라미터가 들어있다. test_console 은 이 JSON 을 직접 읽거나, 16바이트를 헤더에 박아도 된다.

### 2.1 add_int8 golden
```
input  shape=[1,16] dtype=int8 scale=0.007837736 zp=0
output shape=[1,16] dtype=int8 scale=0.007839226 zp=-128

input  bytes : f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
output bytes : f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
```

### 2.2 identity_int8 golden
```
input  shape=[1,16] dtype=int8 scale=0.007837736 zp=0
output shape=[1,16] dtype=int8 scale=0.007837736 zp=0

input  bytes : f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
output bytes : f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
```

### 2.3 Quantization 공식 (디버깅 시 real value 확인용)
```
real      = (int8_value - zero_point) * scale
int8_value = clip(round(real / scale) + zero_point, -128, 127)
```

---

## 3. test_console 권장 흐름

```
[1] 디바이스 핸들 오픈      CreateFile("\\?\ApexDevice0", ...)  또는 KMDF symbolic link
[2] 모델 로딩              .tflite 파일 → driver IOCTL 로 등록
[3] 인터프리터 init        memory 매핑, MMU map, runControl 셋업
[4] infeed 버퍼 write      golden.input 16바이트 (또는 임의 입력)
[5] invoke / kick          drvier IOCTL 또는 도어벨 레지스터로 칩 trigger
[6] outfeed 버퍼 read      칩이 채워준 16바이트 회수
[7] golden 비교            byte-단위 memcmp → pass/FAIL
[8] real value print       (옵션) per-element 표 — quantize 오차 확인
[9] 디바이스 핸들 close   인터럽트 disable, MMU unmap, register close
```

각 단계 실패 시 의심 대상:

| 단계 | 실패 시 의심 |
|---|---|
| 1 | 디바이스 enumerate, KMDF DEVICE_INTERFACE GUID, ACL/권한 |
| 2 | IOCTL 정의, 모델 buffer copy (METHOD_BUFFERED vs DIRECT), 모델 size 검증 |
| 3 | DMA common buffer 할당, MMU page table, BAR mapping, 인터럽트 라우팅 |
| 4 | infeed FIFO 주소, write barrier, cache flush (writeback) |
| 5 | runControl 레지스터 offset, 도어벨/MMIO write timing |
| 6 | outfeed FIFO drain, cache invalidate, completion 인터럽트 동기화 |
| 7 | byte order, alignment, partial fill, off-by-one |
| 8 | (소프트웨어 비교 단계라 KMDF 와 무관) |
| 9 | unmap leak, 인터럽트 disable 누락, 후속 open 실패 |

---

## 4. 칩이 실제로 일했는지 보는 방법

`run_add_infer.py` 실행 시 stderr 로그(`logs/add_int8_edgetpu_*.log`) 에 libedgetpu 가 칩에 쓴 레지스터 시퀀스가 다 찍힌다. test_console 도 동일한 패턴을 따라야 한다.

### 4.1 Run 단계의 핵심 레지스터 (offset 은 device 절대 주소)
```
0x441d8  infeedRunControl              ← infeed 엔진 RUN
0x44218  outfeedRunControl             ← outfeed 엔진 RUN
0x400c0  opRunControl       (tile)     ← 연산 엔진 RUN
0x40150  narrowToWideRunControl (tile) ← int8 → 내부폭 (dequant 경로)
0x40110  wideToNarrowRunControl (tile) ← 내부폭 → int8 (quant 경로)
0x44018  scalarCoreRunControl
0x44158  avDataPopRunControl
0x44198  parameterPopRunControl
0x40250/0x40298/0x402e0/0x40328  meshBus 0~3 RunControl
0x40190/0x401d0  ringBusConsumer 0~1 RunControl
0x40210  ringBusProducerRunControl
0x48788  tileconfig0 (broadcast 0x7f, write 후 readback 검증)
```
모두 `value=0x2` (Run) 로 write. 종료 시 인터럽트 disable 후 디바이스 close.

### 4.2 칩이 안 돌면 나타날 증상
- 출력 버퍼가 호스트가 초기화한 값 그대로 (보통 `00..00` 또는 garbage)
- runControl readback 이 `0x0` 또는 timeout
- `tileconfig0` write 후 readback 불일치 → tile 활성화 실패
- outfeed completion 인터럽트가 안 옴 → polling 으로 hang

---

## 5. test_console 검증 체크리스트

### 5.1 identity_int8 (1단계)
- [ ] golden.input 16바이트 → infeed
- [ ] invoke
- [ ] outfeed 16바이트 → host buffer
- [ ] `memcmp(out, golden.output, 16) == 0`
- [ ] 실패 시: DMA, MMU map, cache coherency, FIFO offset 점검

### 5.2 add_int8 (2단계, identity 통과 후)
- [ ] 동일하게 16바이트 in → 16바이트 out
- [ ] `memcmp(out, golden.output, 16) == 0`
- [ ] (옵션) per-element real value 출력해서 `out_real ≈ in_real + 1.0` 확인 (오차 ≤ 0.5 × output_scale ≈ 0.0039 정상)
- [ ] 실패 시: ADD op 매핑, 상수 텐서 로딩, requantize 회로, saturation

### 5.3 회귀 테스트용 변형 입력 (golden 의존 없는 추가 검증)
identity 는 **모든** 입력이 출력=입력으로 나와야 함. add 는 다음과 같이 검증 가능:

```
입력 byte = b  (int8 = b 그대로 부호 해석)
입력 real = b * 0.007837736
기대 real = b * 0.007837736 + 1.0
기대 출력 = clip(round((b * 0.007837736 + 1.0) / 0.007839226) + (-128), -128, 127)
```

엣지 케이스로 권장:
- `0x80` (-128, 최소값) → 입력 real ≈ -1.003 → 출력 real ≈ -0.003 → 출력 byte 약 `0x80`
- `0x7f` (+127, 최대값) → 입력 real ≈ +0.995 → 출력 real ≈ +1.995 → 출력 byte 약 `0x7e`/`0x7f` (saturation 근방)
- `0x00` → 출력 real ≈ +1.003 → 출력 byte = `0x00` (zp 시프트 효과 직접 확인)

saturation 경계에서 칩이 overflow 처리 못 하면 wrap-around 가 보임.

---

## 6. 컴파일 가능 여부 사전 확인

`add_int8.tflite` / `identity_int8.tflite` → `*_edgetpu.tflite` 변환은 Linux x86_64 에서 `edgetpu_compiler` 로 한다 (Windows native 없음 → WSL/Docker).

```bash
edgetpu_compiler -s -o ./model ./model/add_int8.tflite
edgetpu_compiler -s -o ./model ./model/identity_int8.tflite
```

생성되는 `*_edgetpu.log` 의 이 줄이 핵심:
```
Number of operations that will run on Edge TPU:  N
Number of operations that will run on CPU:       M
```

- `N ≥ 1, M = 0` 이상적. 칩이 모든 산술을 처리.
- `M > 0` 이면 일부 op 이 CPU fallback. test_console 검증 의미가 약해지므로 모델을 fully-int8 로 다시 export 하거나 그래프를 수정해야 함.

현재 환경에서는 `add_int8_edgetpu.tflite` 가 정상 컴파일됐고 (`run_add_infer.py` 의 byte 일치 통과로 확인), `narrowToWide`/`wideToNarrow`/`opRunControl` 이 모두 활성화되는 로그가 찍혔다 — TPU 매핑 정상.

---

## 7. KMDF 측 IOCTL 인터페이스 제안 (참고)

```c
#define IOCTL_TPU_LOAD_MODEL     CTL_CODE(..., 0x800, METHOD_IN_DIRECT,  FILE_WRITE_ACCESS)
#define IOCTL_TPU_INVOKE         CTL_CODE(..., 0x801, METHOD_BUFFERED,   FILE_WRITE_ACCESS)
#define IOCTL_TPU_INFEED_WRITE   CTL_CODE(..., 0x802, METHOD_IN_DIRECT,  FILE_WRITE_ACCESS)
#define IOCTL_TPU_OUTFEED_READ   CTL_CODE(..., 0x803, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#define IOCTL_TPU_RESET          CTL_CODE(..., 0x804, METHOD_BUFFERED,   FILE_WRITE_ACCESS)
```

- `METHOD_IN_DIRECT` / `METHOD_OUT_DIRECT` 로 큰 버퍼는 MDL 기반 zero-copy.
- 16바이트 같은 작은 in/out 은 `METHOD_BUFFERED` 로 단순화해도 무방.
- 모든 IOCTL 은 input size validation 필수 (보안 + 안정성).

---

## 8. 진행 권장 순서

1. **edgetpu_compiler 로그** 확인 → identity/add 둘 다 N≥1, M=0 인지 확인.
2. **`run_add_infer.py` 통과** 재확인 (이미 통과). 이건 pycoral + 공식 libedgetpu 경로의 reference 동작.
3. **KMDF test_console 작성** → identity 모델로 byte-passthrough 통과시키기.
4. identity 통과 → **add 모델로 산술 + requantize 통과**.
5. 둘 다 통과 → 큰 모델 (`ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite`) 로 확대.
6. 큰 모델까지 통과하면 KMDF outfeed 경로의 정합성은 확보된 것.

각 단계에서 stderr 로그 (`logs/*.log`) 의 레지스터 시퀀스를 reference 로 두고, KMDF 측 로그와 offset/순서를 비교하면 디버깅이 빠르다.

---

## 부록 A. 파이썬 reference 스크립트
- `run_infer.py` — pycoral + SSD 얼굴 검출. 큰 모델로 전체 파이프라인 동작 확인용.
- `run_add_infer.py` — add_int8 / identity 같은 검증용 단순 모델. test_console 동작과 1:1 비교.
  - 기본: Edge TPU (`add_int8_edgetpu.tflite`)
  - `--cpu` 플래그: tflite_runtime CPU fallback (TPU 안 거치고 ground truth 확인용)

## 부록 B. 자주 헷갈리는 포인트
- **출력 byte 가 입력 byte 와 같다고 칩이 일 안 한 게 아니다.** add 모델은 zp 시프트 때문에 의미가 완전히 다른 real value 를 같은 byte 로 표현할 뿐. real value 표를 찍어서 확인.
- **identity 모델의 그래프는 실제로 `MAXIMUM(x, x)`** (TFLite converter 가 `tf.identity` 를 통째로 제거하기 때문에 op 1개 강제 잔존용). 의미는 `y = x`.
- **edgetpu_compiler 는 Linux 전용.** Windows 에서는 WSL/Docker 필수.
- **byte 일치 == 칩 동작 정상.** 호스트 메모리에 칩이 직접 DMA write 한 결과를 비교하는 것이므로, byte 가 맞으면 infeed/compute/outfeed 경로 전체가 정상.
