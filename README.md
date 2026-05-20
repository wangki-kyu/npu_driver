# npu_driver — Custom Windows Kernel Driver for Coral Edge TPU

> A from-scratch Windows KMDF driver for the Google Coral M.2 Edge TPU (Apex/Beagle chip),
> reverse-engineered from libedgetpu + gasket-driver, with a C ABI runtime DLL and
> real-time face detection demos. No vendor driver, no SDK — raw PCIe MMIO up.

[데모 영상](https://www.youtube.com/watch?v=FChT0XPICRc) · [Notion 포트폴리오](#) · [Technical deep dives](#기술-하이라이트)

[![Demo: real-time face detection on a custom Coral Edge TPU Windows driver](https://img.youtube.com/vi/FChT0XPICRc/maxresdefault.jpg)](https://www.youtube.com/watch?v=FChT0XPICRc)

> ▲ 클릭하면 YouTube 데모 영상으로 이동합니다.

---

## TL;DR

Google 의 Coral M.2 Edge TPU 는 **Linux 용 (libedgetpu) 만** 공식 지원합니다. 이 프로젝트는 그 칩을 **Windows 에서 직접 구동**하기 위해:

1. libedgetpu / gasket-driver / coral.sys 를 reverse 해서 PCIe MMIO·IOCTL·DMA 흐름을 복원하고
2. **KMDF 커널 드라이버 (`npu_driver.sys`)** 를 from-scratch 작성,
3. 그 위에 **C ABI 런타임 DLL (`npu_runtime.dll`)** 을 얹어 Python / C# / Rust 어디서든 호출 가능하게 만들고,
4. SSD MobileNet v2 face 모델로 **실시간 얼굴 검출 데모** 까지 end-to-end 완성

한 프로젝트입니다. 추론 결과는 pycoral (공식 SDK) 와 **byte-identical** 검증 완료.

---

## 왜 만들었나

- 목표: **Windows 커널 드라이버 + NPU 하드웨어** 를 개발하여 부족한 실무 능력을 채우기 위함 
- Coral M.2 는 시중에서 구매 가능한 NPU 라 학습용으로 선택 (실제 상용 NPU 가속기와 동일한 PCIe·DMA·MMU 패턴)
- Google 의 driver 는 Linux 전용 → Windows 에는 driver 자체가 없음 → reverse 부터 자작 필요
- 그 과정에서 ring buffer wrap 버그, SSD quant 표현 등 **비자명한 hardware-level 문제를 직접 진단/해결**

---

## 아키텍처

```
  ┌─────────────────────────────────────────────────────────────┐
  │  Application (Python ctypes / C# DllImport / Rust FFI)      │
  │  - npu_runtime_demo.py     (단일 이미지)                      │
  │  - npu_runtime_camera.py   (webcam / 영상)                   │
  │  - npu_runtime_gui.py      (PySide6 GUI + 온도 표시)          │
  └──────────────────────────────┬──────────────────────────────┘
                                  │  C ABI (init / infer / temperature / free)
  ┌──────────────────────────────▼──────────────────────────────┐
  │  npu_runtime.dll  (C ABI wrapper, static CRT)                │
  │  - .tflite 파싱 (FlatBuffers)                                 │
  │  - SSD 후처리: Relayout → Dequant → DecodeBoxes → NMS         │
  └──────────────────────────────┬──────────────────────────────┘
                                  │  DeviceIoControl (IOCTL)
  ┌──────────────────────────────▼──────────────────────────────┐
  │  npu_driver.sys  (KMDF kernel-mode driver)                   │
  │  - PCIe BAR2 CSR mapping / MSI-X interrupt (ISR + DPC)        │
  │  - Page table 관리 (simple + extended VA)                     │
  │  - Descriptor ring queue / PARAM caching                     │
  │  - Thermal sensor (OMC0)                                     │
  └──────────────────────────────┬──────────────────────────────┘
                                  │  PCIe Gen2 x1  (MMIO + DMA)
  ┌──────────────────────────────▼──────────────────────────────┐
  │  Coral M.2 Edge TPU silicon  (Apex / Beagle, 0x1ac1:0x089a)  │
  └─────────────────────────────────────────────────────────────┘
```

데이터 흐름 (1 회 추론):
```
RGB 320×320 → INFEED → 칩 추론 → OUTFEED (TYXZ tile-interleaved)
   → Relayout (→ linear YXZ) → Dequantize (uint8→float)
   → DecodeBoxes (anchor + offset) → NMS → list[Detection]
```

---

## 저장소 구조

```
npu_driver/
├── npu_driver/                  ★ KMDF 커널 드라이버 (npu_driver.sys)
│   ├── Driver.c                 - DriverEntry, WDF 초기화
│   ├── Device.c                 - PrepareHardware (BAR map, MSI-X, thermal enable)
│   ├── Memory.c                 - contiguous alloc, page table 매핑
│   ├── Queue.c                  - IOCTL dispatch (ALLOC / INFER / PARAM / TEMP ...)
│   └── Hardware.h               - 전체 CSR offset + 비트 레이아웃 정의
│
├── npu_runtime/                 ★ C ABI 런타임 DLL (npu_runtime.dll)
│   ├── npu_runtime.h            - public C ABI
│   └── npu_runtime.cpp          - init / infer / temperature / free 구현
│
├── include/                     공용 header-only 라이브러리
│   ├── Public.h                 - IOCTL 코드 + struct 정의
│   ├── util.hpp                 - 디바이스 핸들 열기 (SetupAPI)
│   ├── apex_model_fb.hpp        - .tflite (FlatBuffers) 파서 + VA 패치
│   ├── apex_postprocess.hpp     - Relayout / TransformSignedDataType
│   ├── apex_ssd_postprocess.hpp - Dequantize / DecodeBoxes / NMS
│   └── flatbuffers/             - FlatBuffers 런타임 (vendored)
│
├── infer_test_console/          SSD 추론 end-to-end 검증 콘솔 (DLL 의 원형)
├── add_model_test_console/      ADD 모델 (최소 모델) 동작 검증
├── npu_test_console/            초기 IOCTL 단위 테스트
│
├── model/                       ssd_mobilenet_v2_face_*.tflite + MODELS.md
├── assets/                      테스트 이미지 (karina.jpg 등)
├── reference/                   reverse 분석 노트 (CORAL_KMDF_REFERENCE 등)
└── END_TO_END.md                전체 파이프라인 상세 문서 (14 챕터)
```

> **참고:** Python 데모 (`npu_runtime_demo.py` / `npu_runtime_camera.py` / `npu_runtime_gui.py`) 는
> 별도 디렉터리(`edge_tpu_test/`)에 있으며 `npu_runtime.dll` 을 ctypes 로 로드합니다.

---

## Reverse Engineering 방법론

register 시퀀스를 추측으로 포팅하는 대신, **reference 구현 (libedgetpu) 을 직접 계측해서
실제 동작을 정답(ground truth)으로 캡처하고, npu driver 를 그것과 byte 단위로 맞추는** 방식으로 진행했습니다.

**1. libedgetpu fork + 계측 (instrumentation)**
- Google libedgetpu 를 fork, **Windows 빌드 환경을 직접 수정**해서 `edgetpu.dll` 산출
- `verbosity=10` + 직접 삽입한 dump 코드로 **실제 추론 중** 다음을 캡처:
  - 모든 **CSR write** (register offset + value) — 칩 init / 추론 시퀀스의 정답
  - **INFEED** (전처리된 input) / **OUTFEED** (raw TPU tensor)
  - **PARAM** (weight blob, ~6MB) — CRC32 + HEAD/MID/TAIL 형식
- → 칩이 "원래 어떻게 동작해야 하는가" 의 정답지 확보

**2. npu driver / test console 에 동일 포맷 dump 심기**
- 자작 driver 와 test console 에 **byte-by-byte 비교 가능한 동일 dump 포맷** 삽입
  (CRC32 `poly 0xEDB88320` + HEAD/MID/TAIL 32B)
- 동일 tag (`[CSR@...]`, `[PARAM-DUMP]`, `[INPUT-DUMP]`, `[OUT-DUMP]`) 로 양쪽 grep → diff

**3. byte-identical 비교 + mismatch 추적**
- 양쪽 출력의 **size + CRC32 가 일치** → 자작 구현이 libedgetpu 와 byte-perfect 호환임을 증명
- mismatch 시 **첫 다른 byte offset 까지 binary search** 로 좁혀서 정확히 어느 단계가 어긋났는지 격리
- 이 방식으로 FlatBuffer 파싱 / VA 패치 / Relayout / Dequant 를 단계별로 검증

> 이 "역 리버싱" 접근 덕분에 칩 동작을 추측 없이 복원할 수 있었고,
> ring wrap 같은 미세 버그도 "정답지 대비 어디서 처음 어긋나는가" 로 빠르게 격리할 수 있었습니다.

---

## 빌드

### 요구 사항
- Windows 11 x64
- Visual Studio 2022 + **WDK (Windows Driver Kit)**
- 테스트 대상: Coral M.2 Accelerator (PCIe, `0x1ac1:0x089a`)
- 테스트 서명 모드 (`bcdedit /set testsigning on`) — self-signed driver 로딩용

### 드라이버 + DLL 빌드
```powershell
# 전체 솔루션
msbuild npu_driver.sln /p:Configuration=Release /p:Platform=x64

# 또는 개별
msbuild npu_driver\npu_driver.vcxproj   /p:Configuration=Release /p:Platform=x64
msbuild npu_runtime\npu_runtime.vcxproj /p:Configuration=Release /p:Platform=x64
```

산출물:
- `x64\Release\npu_driver.sys` + `.inf` + `.cat`
- `npu_runtime\x64\Release\npu_runtime.dll` (+ `.lib`, `npu_runtime.h`)

### 드라이버 설치
장치 관리자 → Coral 장치 → 드라이버 업데이트 → `npu_driver.inf` 지정
(또는 `pnputil /add-driver npu_driver.inf /install`)

### Python 데모 실행
```powershell
pip install opencv-python numpy PySide6
python npu_runtime_demo.py  assets\karina.jpg       # 단일 이미지
python npu_runtime_camera.py crowd.mp4 --save out.mp4  # 영상 + 저장
python npu_runtime_gui.py                            # GUI (온도 표시 포함)
```

---

## IOCTL 인터페이스

`include/Public.h` 에 정의. `DeviceIoControl` 로 호출.

| IOCTL | 코드 | 역할 |
|---|---|---|
| `IOCTL_ALLOC_IO_BUFFERS` | 0x806 | input/output/param/bitstream 슬롯 contiguous 할당 + PTE 매핑 |
| `IOCTL_INFER_NEW`        | 0x808 | descriptor ring 에 추론 enqueue + 완료 대기 (IRQ) |
| `IOCTL_PARAM_CACHE_NEW`  | 0x809 | 칩 SRAM 으로 weight caching (모델당 1회) |
| `IOCTL_FREE_IO_BUFFERS`  | 0x807 | 슬롯 해제 + PTE unmap |
| `IOCTL_GET_TEMPERATURE`  | 0x80A | OMC0 thermal sensor ADC 읽어 millidegree C 반환 |
| `IOCTL_MAP_BUFFER` / `IOCTL_UNMAP_BUFFER` | 0x801/2 | 임의 user buffer 의 PTE 매핑 |

---

## C ABI (`npu_runtime.h`)

```c
npu_status_t npu_runtime_init_face_ssd(const char* model_path, const char* anchors_path,
                                       npu_handle_t* out_handle);
npu_status_t npu_runtime_get_input_size(npu_handle_t h, uint32_t* w, uint32_t* h_, uint32_t* c);
npu_status_t npu_runtime_infer_image(npu_handle_t h, const uint8_t* rgb, uint32_t len,
                                     npu_detection_t* out, uint32_t max, uint32_t* count);
npu_status_t npu_runtime_get_temperature(npu_handle_t h, float* celsius, uint32_t* raw_adc);
void         npu_runtime_free(npu_handle_t h);
```

설계:
- **Opaque handle** — 내부 상태 은닉, ownership 명확
- **Caller-allocated output buffer + count** — 언어 무관 (메모리 ownership 이 caller 에 고정, free 함수 불필요)
- **UTF-8 `const char*`** — wchar_t 보다 cross-language 친화
- **Exception-safe** — 모든 함수가 `try/catch` 로 C++ 예외를 C error code 로 변환 (`extern "C"` 경계 밖 누출 금지)
- **Static CRT** — VC++ Redistributable 없이 DLL 한 개만 배포

Python 호출 예 (10 줄):
```python
import ctypes
dll = ctypes.CDLL("npu_runtime.dll")
# ... argtypes 설정 ...
h = ctypes.c_void_p()
dll.npu_runtime_init_face_ssd(b"model.tflite", b"anchors.bin", ctypes.byref(h))
dll.npu_runtime_infer_image(h, rgb_bytes, len(rgb_bytes), buf, 50, ctypes.byref(n))
```

---

## 기술 블로그
https://velog.io/@wang_ki/series/windowsdrivernpu

---

## 현재 상태 & 알려진 한계 (의도된 scope)

- ✅ SSD MobileNet v2 face 모델 end-to-end 동작, 실시간 (~30+ FPS) 추론
- ✅ pycoral 와 score / box byte-identical
- ⚠️ **단일 모델 family hardcoded** — 다른 모델(YOLO 등)은 미지원 (scope 결정)
- ⚠️ **box 좌표 ~20px 오차** — `TFLite_Detection_PostProcess` 의 custom_options scale factor
  정확값 미추출, 표준값 `(10,10,5,5)` 사용. score 는 정확. (known limitation)
- ⚠️ **handle 당 single-thread** 가정 (multi-thread mutex 미구현)

> 위 한계들은 "학습/포트폴리오 목적의 의도된 scope" 이며, 핵심인 driver·DMA·MMU·IOCTL
> 흐름은 완결되어 있습니다.

---

## 레퍼런스 / 출처

reverse engineering 의 기반이 된 공개 소스 (직접 빌드/분석):
- **[libedgetpu (fork)](https://github.com/wangki-kyu/libedgetpu)** ← ★ 본인 fork. Windows 빌드 환경 수정 + CSR/buffer dump 계측 추가.
  byte-identical 비교의 정답지(ground truth) 생성에 사용. *(원본: [google-coral/libedgetpu](https://github.com/google-coral/libedgetpu))*
- [gasket-driver](https://github.com/google/gasket-driver) — Linux kernel driver (CSR 시퀀스, thermal sensor)
- coral.sys — Microsoft-signed 참조 driver (동작 비교용)

모델: `ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite` (Coral 공식 모델)

---

## 라이선스 / License

이 저장소의 **자작 코드** — `npu_driver.sys`, `npu_runtime.dll`, `apex_*.hpp` 후처리 라이브러리,
Python 데모 — 는 **Apache License 2.0** 하에 배포됩니다. (`LICENSE` 파일 참조)

### 서드파티 / 출처

| 구성요소 | 라이선스 | 비고 |
|---|---|---|
| FlatBuffers (`include/flatbuffers/`) | Apache 2.0 | vendored — 원본 LICENSE 유지 |
| libedgetpu | Apache 2.0 | chip init / DMA / FlatBuffer schema **분석 참고** |
| gasket-driver | **GPL-2.0** | CSR 시퀀스 / thermal calibration 등 **하드웨어 사실 참고** |
| 모델 `ssd_mobilenet_v2_face_*` | Apache 2.0 | Coral 공식 모델 |
---


