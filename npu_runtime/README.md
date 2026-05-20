# npu_runtime — Coral Edge TPU C ABI 런타임 DLL

> 자작 KMDF 드라이버(`npu_driver.sys`)의 IOCTL 흐름을 감싸,
> Python / C# / Rust / C++ 어디서든 함수 몇 개로 SSD 얼굴 검출을 호출할 수 있게 한 **C ABI DLL** 입니다.
>
> 전체 프로젝트 개요는 상위 [`../README.md`](../README.md) 참고.

---

## 목표

`.tflite` 모델 로드 → 칩 버퍼 할당 → 추론 → SSD 후처리까지 한 호출(`npu_runtime_infer_image`)로 끝납니다.
내부적으로 `infer_test_console` 의 동작을 그대로 따르되, 진단용 dump 를 모두 제거해 GUI·서비스에서 바로 쓸 수 있게 정리한 버전입니다.

```
RGB 320×320 uint8
   → [DLL] 입력 슬롯 복사 → IOCTL_INFER_NEW (칩 추론)
   → Relayout (TYXZ→YXZ) → Dequantize → DecodeBoxes → NMS
   → npu_detection_t[]  (class_id / score / ymin,xmin,ymax,xmax)
```

---

## C ABI (`npu_runtime.h`)

```c
const char*  npu_runtime_get_version(void);
const char*  npu_runtime_get_last_error(npu_handle_t h);

npu_status_t npu_runtime_init_face_ssd(const char* model_path_utf8,
                                       const char* anchors_bin_path_utf8,
                                       npu_handle_t* out_handle);
npu_status_t npu_runtime_get_input_size(npu_handle_t h,
                                        uint32_t* w, uint32_t* h_, uint32_t* c);
npu_status_t npu_runtime_infer_image(npu_handle_t h,
                                     const uint8_t* image_rgb, uint32_t len,
                                     npu_detection_t* out, uint32_t max,
                                     uint32_t* out_count);
npu_status_t npu_runtime_get_temperature(npu_handle_t h,
                                         float* celsius, uint32_t* raw_adc);
void         npu_runtime_free(npu_handle_t h);
```

상태 코드 (`npu_status_t`): `NPU_OK=0`, `INVALID_ARG`, `DEVICE_NOT_FOUND`, `MODEL_LOAD_FAIL`,
`ALLOC_FAIL`, `IOCTL_FAIL`, `INFER_FAIL`, `ANCHORS_LOAD_FAIL`, `INTERNAL=99`.
실패 시 사람이 읽을 메시지는 `npu_runtime_get_last_error(h)` 로.

### 설계 원칙
- **Opaque handle** (`npu_handle_t`) — 내부 상태 은닉, ownership 명확
- **Caller-allocated output + count** — 언어 무관, free 함수 불필요
- **UTF-8 `const char*`** — cross-language 친화 (wchar_t 회피)
- **Exception-safe** — 모든 함수가 `try/catch` 로 C++ 예외를 C error code 로 변환 (ABI 경계 밖 누출 금지)
- **Static CRT (`/MT`)** — VC++ Redistributable 없이 DLL 한 개만 배포

---

## 사용 예 (Python ctypes)

```python
import ctypes
dll = ctypes.CDLL("npu_runtime.dll")

class Det(ctypes.Structure):
    _fields_ = [("class_id", ctypes.c_int32), ("score", ctypes.c_float),
                ("ymin", ctypes.c_float), ("xmin", ctypes.c_float),
                ("ymax", ctypes.c_float), ("xmax", ctypes.c_float)]

h = ctypes.c_void_p()
dll.npu_runtime_init_face_ssd(b"model.tflite", b"anchors.bin", ctypes.byref(h))

buf = (Det * 50)()
n = ctypes.c_uint32()
dll.npu_runtime_infer_image(h, rgb_bytes, len(rgb_bytes), buf, 50, ctypes.byref(n))
# buf[:n.value] 사용

dll.npu_runtime_free(h)
```

---

## 빌드

```powershell
msbuild npu_runtime\npu_runtime.vcxproj /p:Configuration=Release /p:Platform=x64
```

산출물: `npu_runtime\x64\Release\npu_runtime.dll` (+ `.lib`, `npu_runtime.h`)

후처리·모델 파싱 로직은 `../include/` 의 header-only 라이브러리
(`apex_model_fb.hpp`, `apex_postprocess.hpp`, `apex_ssd_postprocess.hpp`, `util.hpp`, `Public.h`) 에 의존합니다.
런타임에는 `npu_driver.sys` 가 설치된 Coral M.2 (`0x1ac1:0x089a`) 가 필요합니다.

---

## 현재 범위 (v0.1.0)

- ✅ SSD MobileNet v2 face 모델 (320×320 RGB uint8) end-to-end
- ✅ 칩 온도 읽기 (`npu_runtime_get_temperature`, OMC0 thermal sensor)
- ⚠️ **단일 모델 family hardcoded** — quant param / scale factor 가 face SSD 기준 고정
- ⚠️ **handle 당 single-thread** 가정 (내부 lock 없음)
- anchors, quant params가 하드코딩되어 있어 다른 모델 사용 불가능(추후 개선 예정)
