# npu_driver End-to-End Guide

> Coral M.2 PCIe Edge TPU 를 Windows 에서 KMDF driver + standalone post-processing 만으로
> face detection 까지 수행하는 시스템의 전 구간 해설.
> pycoral / libedgetpu / tflite_runtime 0 byte. 추론 path 는 전부 직접 구현.

작성일: 2026-05-18 (Karina face detection 성공 직후)
대상 독자: 본인 (몇 달 뒤에 다시 봐도 이해 가능하게) + 면접관

---

## 0. 한 화면 요약

```
JPEG 이미지
   │
   ▼ (1) infer_test_console.cpp : WIC 로 letterbox resize → 320×320×3 uint8
JPEG → 320×320 RGB uint8 buffer
   │
   ▼ (2) apex_fb::LoadModel : tflite 파일 파싱 → ApexModelFb struct
       (bitstream, patches, layers, exe0/exe1 parameters)
   │
   ▼ (3) IOCTL_ALLOC_IO_BUFFERS : KMDF driver 가 DMA-coherent buffer 할당
       → host VA ↔ device VA 매핑 테이블 setup
   │
   ▼ (4) host buffer 에 input + bitstream + parameters 채움
   │
   ▼ (5) IOCTL_INFER_NEW : chip 에 "go" → chip 이 OUTPUT buffer 에 결과 쓰기
   │
   ▼ (6) chip raw OUTFEED 읽기 (8136B Squeeze1 + 8136B convert_scores)
   │     anchor 마다 4 byte (실제 2 byte + z-padding 2 byte)
   │
   ▼ (7) apex_pp::Relayout + TransformSignedDataType
       TYXZ tile-interleaved padded → linear YXZ uint8
       결과: 8136B Squeeze1 + 4068B convert_scores
   │
   ▼ (8) apex_pp::Dequantize : uint8 → float32
       Squeeze1  : (byte-144) * 0.10822763 → (dy, dx, dh, dw)
       scores    : byte / 256              → (bg, face) ∈ [0,1]
   │
   ▼ (9) apex_pp::DecodeBoxes : anchor (모델 const) + (dy, dx, dh, dw) → (ymin, xmin, ymax, xmax)
   │
   ▼ (10) apex_pp::NonMaxSuppression : score ≥ 0.5 인 box 중 IoU 기준 1 개 선택
   │
   ▼ (11) C:\temp\our_detections.txt 에 결과 저장
   │
   ▼ (12) draw_our_detections.py : letterbox 역변환 → 원본 이미지에 OpenCV 박스 그리기
   │
   ▼ karina.jpg 의 얼굴에 빨간 박스 + "id=0 0.996" ✓
```

---

## 1. 디렉토리 구조

```
E:\work\project\npu_driver\
├── npu_driver\              ← KMDF kernel driver (.sys)
│   ├── Driver.c             ← DriverEntry, WDF setup
│   ├── Device.c             ← PCI device init, BAR mapping, CSR setup, ISR
│   ├── Queue.c              ← IOCTL handlers (ALLOC_IO_BUFFERS, INFER_NEW, ...)
│   ├── Memory.c             ← DMA-coherent buffer, PTE 매핑
│   └── Hardware.h           ← CSR offset 정의 (Apex chip register map)
│
├── include\                 ← user/kernel 공통 + 우리 hpp 들
│   ├── Public.h             ← IOCTL 코드 + struct 정의 (driver ↔ app 인터페이스)
│   ├── executable_generated.h  ← libedgetpu/executable.fbs → flatbuffer C++ generated
│   ├── apex_model_fb.hpp    ← tflite 파일 파싱 + LayerInfo 추출 (★ 우리 작성)
│   ├── apex_postprocess.hpp ← Relayout + TransformSignedDataType (★ 우리 작성)
│   └── apex_ssd_postprocess.hpp ← Dequant + DecodeBoxes + NMS (★ 우리 작성)
│
├── infer_test_console\      ← user-mode test app (.exe)
│   └── infer_test_console.cpp ← main: model load → IOCTL → 결과 dump
│
├── npu_test_console\        ← chip CSR 직접 read/write 진단용
├── add_model_test_console\  ← 모델만 load 해서 schema 검증
└── logs\                    ← chip CSR dump, libedgetpu 비교 binary 들
```

부속 도구 (`E:\work\project\edge_tpu_test\`):

```
run_infer.py              ← pycoral 으로 추론 (검증/비교 baseline)
extract_postproc_deps.py  ← tflite 모델에서 anchors + quant params 추출
inspect_tflite_graph.py   ← edgetpu delegate 로 graph 구조 확인
draw_our_detections.py    ← our_detections.txt + 원본 이미지 → 박스 그린 jpg
```

---

## 2. tflite 모델 파일 구조와 파싱

### 2.1 파일 형식

Coral 모델은 표준 tflite (TFL3 header) 인데, 일반 tflite 와 달리 **DWN1 (DarwiNN) signature 가 중간에 박혀있음**:

```
file offset 0x0000  "TFL3" header  (FlatBufferBuilder root = tflite.Model)
                    Model.buffers[]: weights / constants (anchors 도 buffer[1] 에)
                    Model.subgraphs[0]:
                      tensors[]:   convert_scores, Squeeze1, anchors, ...
                      operators[]: edgetpu-custom-op, DEQUANTIZE, TFLite_Detection_PostProcess
                      opcodes[]:   builtin_code 또는 custom_code

file offset ~0x8700 "DWN1" Package signature
                    libedgetpu/package.fbs 정의의 Package
                    serialized_multi_executable: 진짜 chip-runnable 데이터
                        Executable[0] "model"       ← main inference
                          instruction_bitstreams[0] (200KB)
                          field_offsets[]            ← PARAM/INPUT/OUTPUT VA patch 위치
                          input_layers / output_layers (메타데이터)
                          parameters: 197KB  ← exe0 PARAM data
                        Executable[1] "Unknown"     ← parameter caching
                          instruction_bitstreams[0] (~10KB)
                          parameters: 6MB           ← exe1 PARAM data (SRAM 채우기)
```

### 2.2 파싱 로직

`apex_model_fb.hpp::LoadModel(path)` (line 66~) 가 다음을 함:

1. 파일 binary 로 read
2. DWN1 signature 위치 찾기 (line ~146)
3. `GetPackage(buf + dwn1_offset)` 으로 Package 구조 access
4. `MultiExecutable` → `Executable[0]`, `Executable[1]` 각각 추출
5. 각 Executable 의 `field_offsets` → `FieldPatch[]` 로 정리 (instruction bitstream 안의 어느 byte 가 어떤 VA 로 patch 되어야 하는지)
6. `output_layers` 의 각 Layer 에서:
   - name, size_bytes, y/x/z_dim, data_type, execution_count
   - `OutputLayer::layout()` 의 6 개 vector (★ Relayout 에 필요)

결과 struct (`apex_fb::ApexModelFb` line 41~56):

```cpp
struct ApexModelFb {
    std::vector<uint8_t>    bitstream;            // 200KB exe0 chip instruction
    std::vector<FieldPatch> patches;              // 10 개의 patch point
    std::vector<LayerInfo>  input_layers;
    std::vector<LayerInfo>  output_layers;        // Squeeze1, convert_scores
    std::vector<uint8_t>    exe0_parameters;      // 197KB

    std::vector<uint8_t>    param_bitstream;      // 10KB exe1 param-caching instruction
    std::vector<FieldPatch> param_patches;
    std::vector<uint8_t>    parameters;           // 6MB exe1 weights
};
```

### 2.3 LayerInfo 확장 (★ 2026-05-18 추가)

`apex_model_fb.hpp:34` LayerInfo struct 에 OutputLayout 의 6 vectors 추가:

```cpp
struct LayerInfo {
    // 기본
    std::string name;
    size_t      size_bytes;
    int         y_dim, x_dim, z_dim;
    DataType    data_type;

    // ★ post-processing 용 (output layer 에만 채워짐)
    int  execution_count_per_inference = 1;
    bool has_layout = false;
    std::vector<int32_t> y_to_tile_id;          // chip 의 y → tile ID
    std::vector<int32_t> y_to_local_y_offset;
    std::vector<int32_t> x_to_tile_id;          // chip 의 x → tile ID
    std::vector<int32_t> tile_byte_offset;      // 각 tile 의 chip 메모리 시작 offset
    std::vector<int32_t> x_to_local_byte;
    std::vector<int32_t> x_to_local_y_row_size;
};
```

이 vectors 는 `LoadModel` step 13 (line 327~) 에서 `out_layer->layout()->...->Get()` 로 추출.

→ **Relayout 이 chip 의 TYXZ tile-interleaved bytes 를 linear YXZ 로 변환할 때 정확히 이 6 vectors 가 필요.** libedgetpu 의 `layer_information.cc::GetBufferIndex()` 가 같은 vectors 를 사용.

---

## 3. KMDF driver init + IOCTL 정의

### 3.1 IOCTL 목록 (`include/Public.h` line 7~36)

```c
IOCTL_MAP_BUFFER         // (legacy) user buffer 를 device VA 에 매핑
IOCTL_UNMAP_BUFFER       // (legacy)
IOCTL_INFER              // (legacy) 단순 inference
IOCTL_PARAM_CACHE        // (legacy) PARAM caching
IOCTL_INFER_WITH_PARAM   // (legacy)
IOCTL_ALLOC_IO_BUFFERS   // ★ DMA-coherent buffer 일괄 할당 + VA 매핑
IOCTL_FREE_IO_BUFFERS    // ALLOC 의 cleanup
IOCTL_INFER_NEW          // ★ chip 으로 inference 실행 (현재 메인 경로)
IOCTL_PARAM_CACHE_NEW    // ★ exe1 PARAM caching (chip SRAM 에 6MB 미리 적재)
```

### 3.2 핵심 struct: `IOCTL_ALLOC_IO_BUFFERS_IN/OUT`

```c
struct IOCTL_ALLOC_IO_BUFFERS_IN {
    SIZE_T   InputSize;         UINT64 InputDeviceVA;          // = VA_INPUT
    SIZE_T   OutputBboxSize;    UINT64 OutputBboxDeviceVA;     // = VA_OUTPUT_BBOX
    SIZE_T   OutputScoreSize;   UINT64 OutputScoreDeviceVA;    // = VA_OUTPUT_SCORE
    SIZE_T   ScratchSize;       UINT64 ScratchDeviceVA;
    SIZE_T   Exe0BitstreamSize; UINT64 Exe0BitstreamDeviceVA;  // = VA_INFER_BITSTREAM
    SIZE_T   ParamDataSize;     UINT64 ParamDataDeviceVA;      // = VA_EXE1_PARAM_DATA
    SIZE_T   Exe1BitstreamSize; UINT64 Exe1BitstreamDeviceVA;  // = VA_PARAM_BITSTREAM
    SIZE_T   Exe0ParamSize;     UINT64 Exe0ParamDeviceVA;      // = VA_EXE0_PARAM_DATA
};
struct IOCTL_ALLOC_IO_BUFFERS_OUT {
    UINT64   InputUserVA;       // driver 가 매핑한 host 주소
    UINT64   OutputBboxUserVA;
    UINT64   OutputScoreUserVA;
    ...
};
```

→ user app 이 한 IOCTL 로 **모든 buffer 를 한 번에 요청**, driver 가 DMA-coherent 영역을 할당 + chip 의 device VA 에 PTE 로 매핑한 뒤 host-mapping 된 UserVA 를 돌려줌. 이후 user 는 그 UserVA 에 데이터를 쓰면 chip 이 해당 device VA 로 그 데이터를 읽음.

### 3.3 Driver 의 init phase 들 (Device.c 참조)

- Phase 1: PCI BAR mapping (BAR0=registers, BAR2=CSR region)
- Phase 2: MSI interrupt setup, ISR/DPC 등록
- Phase 3: chip reset + clock 안정화 대기
- Phase 4: Run controller setup (각 engine 의 run/halt control)
- Phase 5: Interrupt enable
- Phase 6: TopLevelInterruptManager CSR (THM warn, MBIST mask, ABM enable, ...)

이 phase 들은 libedgetpu 의 `BeagleTopLevelInterruptManager` / `RunController` 등을 reverse 해서 작성. CSR 별 의미는 `Hardware.h` 의 comment 참조.

---

## 4. VA layout

`infer_test_console.cpp` 상단의 정적 VA 상수 (memory project_va_layout 참조):

```
VA_INPUT             = 0x01080000   (NORMALIZED_INPUT_IMAGE_TENSOR, 300KB)
VA_OUTPUT_BBOX       = 0x01100000   (Squeeze1,        8KB)
VA_OUTPUT_SCORE      = 0x01102000   (convert_scores,  8KB)
VA_EXE0_PARAM_DATA   = 0x01040000   (197KB)
VA_EXE1_PARAM_DATA   = 0x00000000   (6MB,  param caching 시 chip 이 SRAM 으로 흡수)
VA_INFER_BITSTREAM   = (exe0 instruction, patch 후 chip 으로)
VA_PARAM_BITSTREAM   = (exe1 instruction)
VA_SCRATCH           = 0x01200000   (필요시)
```

이 VA 들이 instruction bitstream 안의 patch point 에 들어가서 chip 의 load/store 가 정확한 device 주소로 가게 됨.

> ★ 2026-05-18 발견: VA layout 이 libedgetpu 의 "force-simple" 로그와 byte-identical 일치하지 않으면 PARAM_POP throughput 이 5 배 떨어짐 (chip 의 internal scheduler 가 다른 access pattern 으로 동작하는 듯). 위 값은 그 검증을 통과한 layout.

---

## 5. 입력 준비

### 5.1 letterbox JPEG 로딩 (`infer_test_console.cpp:91` `LoadJpegToRGB`)

pycoral 의 `common.set_resized_input` 과 byte-identical 일치하도록:

1. WIC 로 JPEG decode (`CreateDecoderFromFilename`)
2. `scale = min(320/srcW, 320/srcH)`
3. `newW = srcW*scale`, `newH = srcH*scale`
4. `WICBitmapInterpolationModeFant` 로 resize (PIL.BILINEAR 와 가장 유사)
5. 320×320 buffer 를 0 으로 초기화 후 좌상단 (0,0) 부터 newW×newH 영역에 복사
6. 결과: 24bpp RGB packed buffer (3 byte per pixel)

→ 입력 byte 가 pycoral 과 byte-identical 이어야 chip OUTFEED 도 byte-identical 보장. CRC32 비교로 검증.

### 5.2 chip 으로 전송

```c
memcpy(pInputBuf, rgb_buffer, 307200);  // host 메모리에 쓰면 = chip 메모리 (DMA-coherent)
```

이 시점에 chip 은 아직 모름. 다음 IOCTL_INFER_NEW 가 chip 을 깨움.

---

## 6. instruction bitstream patching + chip 실행

### 6.1 patching

`LoadModel` 이 추출한 `model.patches[]` 가 **bitstream 안의 어느 byte 가 어떤 VA 값으로 채워져야 하는지** 알려줌:

```
patch[0] desc=BASE_ADDRESS_PARAMETER  pos=LO32  offset=0x48
patch[1] desc=BASE_ADDRESS_PARAMETER  pos=HI32  offset=0x58
patch[4] desc=INPUT_ACTIVATION        pos=HI32  offset=0x458 name='normalized_input_image_tensor'
patch[5] desc=INPUT_ACTIVATION        pos=LO32  offset=0x468 name='normalized_input_image_tensor'
patch[6] desc=OUTPUT_ACTIVATION       pos=HI32  offset=0x2da68 name='Squeeze1'
patch[7] desc=OUTPUT_ACTIVATION       pos=LO32  offset=0x2da78 name='Squeeze1'
patch[8] desc=OUTPUT_ACTIVATION       pos=HI32  offset=0x30598 name='convert_scores'
patch[9] desc=OUTPUT_ACTIVATION       pos=LO32  offset=0x305a8 name='convert_scores'
```

user app 이 bitstream 의 사본을 만들고, 각 patch point 에 알맞은 device VA 의 LO32/HI32 32-bit 을 박아넣음. 그 후 patched bitstream 을 device 의 `VA_INFER_BITSTREAM` 영역에 memcpy.

### 6.2 IOCTL_INFER_NEW

driver 의 IOCTL handler (Queue.c) 가:

1. chip 의 instruction queue CSR 에 bitstream VA + size 등록
2. instruction queue 의 "go" 비트 set
3. chip 이 bitstream 을 fetch 하면서 자체 SIMD 코어로 inference 실행
4. chip 이 OUTFEED 단계에서 결과를 `VA_OUTPUT_BBOX`, `VA_OUTPUT_SCORE` 에 write
5. chip 이 complete interrupt → driver 의 ISR/DPC 가 IOCTL 을 unblock
6. user app 으로 control 반환

이때 user 는 `pOutputBboxBuf`, `pOutputScoreBuf` 만 읽으면 됨 (DMA-coherent 라 cache flush 불필요, x86 의 경우 memory ordering 만 보장되면 가시).

---

## 7. chip OUTFEED raw bytes 의 의미

★ 이 챕터가 디버깅 내내 가장 헷갈렸던 부분. 정확히 풀이.

### 7.1 size discrepancy

```
Squeeze1       : chip OUTFEED 8136B,  텐서 shape [1, 2034, 4] uint8 = 8136B  ✓ 일치
convert_scores : chip OUTFEED 8136B,  텐서 shape [1, 2034, 2] uint8 = 4068B  ← 2배!
```

convert_scores 만 size 가 2 배인 이유: **chip 의 SIMD lane width 가 4 byte**. z=2 인 텐서를 chip 이 출력할 때 lane align 을 맞추려고 anchor 당 2 byte 실제 데이터 + 2 byte padding 을 write. Squeeze1 은 z=4 라서 lane 에 딱 맞아 padding 없음.

### 7.2 byte layout 예시 (convert_scores)

```
anchor 0   : ff 00 80 80   ← bg_quant=255, face_quant=0, padding 80 80
anchor 1   : ff 00 80 80
...
anchor 203 : 01 ff 80 80   ← bg_quant=1,   face_quant=255 ★ 얼굴!
anchor 206 : 02 fe 80 80   ← bg_quant=2,   face_quant=254 ★ 얼굴!
...
```

→ 2034 anchor 중 face 가 있는 ~9 개만 "다른" byte. 나머지 2025 개는 모두 `ff 00 80 80` → dump 의 head/tail 256 byte 만 보면 "전부 균일" 로 보임. 이게 **"버그 같은" 정상 출력**.

### 7.3 TYXZ tile-interleaved 의 의미

chip 은 내부적으로 image 를 tile (Y, X, Z 축으로 분할된 작은 block) 단위로 연산. OUTFEED 도 tile 순서대로 byte 가 섞여 나옴 (TYXZ = Tile-major, 그 안에서 Y, X, Z 순서).

host 가 `[Y, X, Z]` linear 순서로 보려면 Relayout 필요.

우리 convert_scores 모델 layout 은 거의 trivial:
- `y_dim=1, x_dim=2034, z_dim=2`, tile 수 16 개
- Relayout 의 효과는 사실상 "각 anchor 의 padded 4 byte 에서 앞 2 byte 만 추출" 로 수렴

---

## 8. apex_postprocess.hpp — Relayout + SignedDataType

`include/apex_postprocess.hpp` 의 핵심 함수 3 개.

### 8.1 `DataTypeSize`, `IsSignedDataType` (line 25~46)

```cpp
inline int DataTypeSize(DataType dt) {
    switch (dt) {
        case FIXED_POINT8: case SIGNED_FIXED_POINT8:  return 1;
        case FIXED_POINT16: case SIGNED_FIXED_POINT16: return 2;
        ...
    }
}
inline bool IsSignedDataType(DataType dt) {
    return dt == SIGNED_FIXED_POINT8 || dt == SIGNED_FIXED_POINT16;
}
```

libedgetpu 의 `layer_information.cc:494` `TensorDataTypeSize` 를 그대로 가져옴. 우리 face 모델은 `FIXED_POINT8` (unsigned uint8).

### 8.2 `Relayout(dst, src, layer)` (line 64~138)

libedgetpu `layer_information.cc:206-376` 의 `OutputLayerInformation::Relayout` 의 standalone port.

핵심 idea:

```cpp
// 각 (y, x, z) element 의 chip raw 안에서의 byte offset 계산
auto get_buf_idx = [&](int y, int x, int z) -> int {
    int tile_id = layer.y_to_tile_id[y] + layer.x_to_tile_id[x];
    int global_tile_byte_offset = layer.tile_byte_offset[tile_id];
    int local_x_byte_offset     = layer.x_to_local_byte[x];
    int local_y_byte_offset     = layer.y_to_local_y_offset[y]
                                * layer.x_to_local_y_row_size[x];
    return global_tile_byte_offset + local_y_byte_offset
         + local_x_byte_offset + z;
};

// 모든 (y, x) 에 대해 z 차원 z_bytes 만 dst 로 복사 (padding 은 skip)
for (int y = 0; y < y_dim; ++y) {
  for (int x_tile : tile_x_sizes) {
    const uint8_t* src_ptr = src + get_buf_idx(y, tile_start_x, 0);
    for (int lx = 0; lx < tile_x_size; ++lx) {
      memcpy(dst, src_ptr, z_bytes);   // z_bytes = layer.z_dim * data_type_size
      dst    += z_bytes;
      src_ptr += z_bytes_padded;       // chip 의 lane-aligned stride
    }
  }
}
```

결과: 8136B chip raw → 4068B linear (convert_scores), 8136B → 8136B no-op-like (Squeeze1).

### 8.3 `TransformSignedDataType(buffer, size, layer)` (line 142~152)

libedgetpu `layer_information.cc:130-149` port. signed quant 인 경우만 MSB XOR:

```cpp
if (!IsSignedDataType(layer.data_type)) return;
int dts = DataTypeSize(layer.data_type);
for (size_t i = dts - 1; i < total_bytes; i += dts) {
    buffer[i] ^= 0x80;    // sign bit flip → uint8 ↔ int8
}
```

우리 face 모델은 FIXED_POINT8 (unsigned) 라 no-op.

### 8.4 검증

`infer_test_console.cpp:802~` 가 Relayout 결과를 `C:\temp\our_post_relayout_*.bin` 으로 저장.
libedgetpu 의 단독 빌드가 만든 `libe_post_relayout_*.bin` / `libe_final_user_*.bin` 과 `fc /b` 비교:

```
MATCH  8136B  libe_post_relayout_Squeeze1       == our_post_relayout_squeeze1
MATCH  4068B  libe_post_relayout_convert_scores == our_post_relayout_convert_scores
```

→ **byte-identical 확정**. Relayout/SignedDataType 가 libedgetpu 와 동일하게 동작.

---

## 9. apex_ssd_postprocess.hpp — Dequant + DecodeBoxes + NMS

`include/apex_ssd_postprocess.hpp` 의 함수들.

### 9.1 `Dequantize` (line 56~62)

```cpp
inline void Dequantize(float* dst, const uint8_t* src, size_t count, QuantParams q) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = (static_cast<int>(src[i]) - q.zero_point) * q.scale;
    }
}
```

우리 모델의 quant params (`extract_postproc_deps.py` 출력으로 확정):

| tensor | scale | zero_point |
|---|---|---|
| Squeeze1 (uint8 [2034,4]) | 0.10822763 | 144 |
| convert_scores (uint8 [2034,2]) | 0.00390625 (=1/256) | 0 |

dequant 결과:
- Squeeze1: `(byte - 144) * 0.10822763` → (dy, dx, dh, dw) per anchor
- convert_scores: `byte / 256` → (bg_score, face_score) per anchor, range [0, 1]

### 9.2 `DecodeBoxes` (line 68~93)

SSD MobileNet 의 표준 anchor + offset decoding:

```cpp
ycenter = ay + (dy / 10.0f) * ah;     // y_scale = 10
xcenter = ax + (dx / 10.0f) * aw;     // x_scale = 10
h       = exp(dh / 5.0f) * ah;        // h_scale = 5
w       = exp(dw / 5.0f) * aw;        // w_scale = 5

out[i*4 + 0] = ycenter - h/2;   // ymin
out[i*4 + 1] = xcenter - w/2;   // xmin
out[i*4 + 2] = ycenter + h/2;   // ymax
out[i*4 + 3] = xcenter + w/2;   // xmax
```

`anchors` 는 모델에 const 로 박혀있고 (`tensor[1] buf=1`, 2034×4 float32), `extract_postproc_deps.py` 로 `C:\temp\anchors.bin` 추출. `LoadAnchorsBin(path)` (line 156~) 이 fread.

★ 알려진 limitation: 우리 출력 box 가 pycoral 대비 ~20 pixel 오차. 원인 추정: scale factors (10, 10, 5, 5) 가 이 모델 한정으로 다를 수 있음. TFLite_Detection_PostProcess op 의 `custom_options` (flexbuffer) 에서 정확한 값 추출하면 해결. 현재 face 가 box 안에 들어가는 수준은 확인됨.

### 9.3 `NonMaxSuppression` (line 113~137)

표준 greedy NMS:

1. 모든 (anchor, class) pair 에 대해 score ≥ threshold (0.5) 인 것만 candidate
2. score 내림차순 정렬
3. 첫 element 채택, 그것과 IoU > threshold (0.6) 인 같은 class 후속 elements 제거
4. 다음 살아남은 element 채택... 반복
5. max_detections (50) 까지

### 9.4 end-to-end pipeline `RunSsdPostprocess` (line 175~)

```cpp
std::vector<Detection> RunSsdPostprocess(
    const uint8_t* squeeze1_uint8,   // 8136B post-Relayout
    const uint8_t* scores_uint8,     // 4068B post-Relayout
    int num_anchors,                 // 2034
    const float* anchors,            // 32544B (loaded from anchors.bin)
    QuantParams q_squeeze1,          // {0.10822763, 144}
    QuantParams q_scores,            // {0.00390625, 0}
    SsdScaleFactors sf,              // {10,10,5,5}
    SsdNmsParams nms                 // {0.5, 0.6, 50, 1}
);
```

내부 흐름:
```
Dequantize(squeeze1)  →  dq_boxes  (float [2034][4])
Dequantize(scores)    →  dq_scores (float [2034][2])
DecodeBoxes(...)      →  decoded   (float [2034][4]  ymin/xmin/ymax/xmax)
NonMaxSuppression()   →  vector<Detection>
```

---

## 10. infer_test_console main flow (요약)

`infer_test_console.cpp:324` `main()` 의 큰 흐름:

```
1. CoInitializeEx (WIC 용)
2. apex_fb::LoadModel(path) → ApexModelFb
3. dump_param() : PARAM 데이터의 CRC32 print (libedgetpu 비교용)
4. FindPicoDriverDevice() → KMDF driver 의 HANDLE
5. IOCTL_ALLOC_IO_BUFFERS : 모든 buffer 할당 + UserVA 받기
6. ★ 2026-05-18 sentinel : 출력 buffer 를 0xCC/0xAA 로 채움
   (chip 이 실제로 덮어쓴 영역과 안 쓴 영역 구분 용)
7. PatchBitstream() : exe0/exe1 bitstream 에 VA 박아넣기
8. memcpy : patched bitstream + parameters + input image → UserVA
9. (조건) IOCTL_PARAM_CACHE_NEW : exe1 PARAM 을 chip SRAM 으로 caching
10. IOCTL_INFER_NEW : chip 실행 (블로킹)
11. (출력 검사) coverage / diff-anchor / hex dump
12. ★ save_bin : C:\temp\our_squeeze1_raw.bin, our_convert_scores_raw.bin
13. ★ apex_pp::RelayoutAndSignedXform : 두 layer 각각 변환
    + C:\temp\our_post_relayout_*.bin 저장
14. ★ SSD postprocess :
    - apex_pp::LoadAnchorsBin("C:\temp\anchors.bin")
    - apex_pp::Dequantize x2 (Squeeze1, scores)
    - C:\temp\our_dq_*.bin 저장
    - apex_pp::DecodeBoxes
    - C:\temp\our_decoded_boxes.bin 저장
    - apex_pp::NonMaxSuppression
    - C:\temp\our_detections.txt 저장 + stdout print
15. cleanup : IOCTL_FREE_IO_BUFFERS, CloseHandle
```

★ 표시가 2026-05-18 추가 부분 (standalone post-processing).

---

## 11. 시각화

`draw_our_detections.py` (E:\work\project\edge_tpu_test\):

1. `cv2.imread(원본 jpg)` → srcW, srcH
2. letterbox 역변환:
   ```
   scale = min(320/srcW, 320/srcH)
   newW, newH = srcW*scale, srcH*scale
   ```
3. `C:\temp\our_detections.txt` parse → `[{id, score, ymin, xmin, ymax, xmax}, ...]`
4. 각 detection 에 대해:
   - normalized [0..1] → 320 pixel → letterbox content 안으로 clip → `/scale` → 원본 픽셀
   - `cv2.rectangle` + `cv2.putText`
5. `cv2.imwrite(원본_our_infer.jpg)`

→ karina.jpg 의 얼굴 위치에 빨간 박스 + "id=0 0.996" 텍스트 ✓

---

## 12. 디버깅 도구 / 검증 framework

작업하면서 만든 진단 도구 정리:

| 도구 | 위치 | 용도 |
|---|---|---|
| `dump_param()` | infer_test_console.cpp | exe0/exe1 PARAM 의 size+CRC32+HEAD/MID/TAIL print (libedgetpu 와 1:1 비교) |
| `DumpHex()` | infer_test_console.cpp | 임의 buffer 의 hex+ASCII dump |
| `save_layer_dump()` | infer_test_console.cpp | output layer bytes 를 txt 로 저장 |
| `[outfeed] coverage` summary | infer_test_console.cpp:830~ | chip 이 sentinel 0xCC 를 얼마나 덮었나 (= 실제 written 영역) |
| `[diff-anchor]` print | infer_test_console.cpp | convert_scores 의 anchor 중 `ff 00 80 80` 와 다른 것만 print |
| `[REFMT-IN]` / `[REFMT-OUT]` dump | libedgetpu/tflite/custom_op.cc:294 (HACK) | libedgetpu 의 ReFormatOutputs 입출력 비교 |
| `[outfeed] head/tail` dump | libedgetpu/driver/single_tpu_request.cc (HACK) | libedgetpu 의 host_outputs_ chip raw bytes |
| `extract_postproc_deps.py` | edge_tpu_test/ | tflite 모델에서 anchors + quant params 추출 |
| `inspect_tflite_graph.py` | edge_tpu_test/ | tflite graph 구조 확인 (op chain, tensor 메타데이터) |
| `extract_op_chain.py` | edge_tpu_test/ | flatc JSON 에서 operator chain 만 추출 (graph 구조 디버깅) |
| `draw_our_detections.py` | edge_tpu_test/ | 우리 detection 결과 시각화 |
| `fc /b` 비교 | PowerShell | 우리 binary vs libedgetpu binary 의 byte-by-byte 일치 검증 |

---

## 13. 검증 방법론 (★ 중요)

이 프로젝트의 신뢰성은 **단계마다 libedgetpu 와 byte-identical 비교** 로 확보. 각 단계의 검증:

| 단계 | 우리 결과 | libedgetpu 결과 | 검증 |
|---|---|---|---|
| 모델 PARAM data | `dump_param` CRC32 | libedgetpu `package_registry.cc:582` CRC32 | CRC32 match ✓ |
| Input bytes (letterbox JPEG) | `INPUT-DUMP` CRC32 | libedgetpu `single_tpu_request.cc:117` CRC32 | CRC32 match ✓ |
| chip OUTFEED raw | `our_squeeze1_raw.bin`, `our_convert_scores_raw.bin` | libedgetpu `single_tpu_request.cc::PostProcessOutputBuffers` dump | byte-identical ✓ |
| post-Relayout | `our_post_relayout_*.bin` | `libe_post_relayout_*.bin` | byte-identical ✓ |
| post-SignedDataType | (no-op for face) | `libe_final_user_*.bin` | byte-identical ✓ |
| score quantized | `our_dq_convert_scores.bin` | pycoral `interpreter.tensor(5)()` (convert_scores1 float32) | element-wise 비교 가능 |
| detection score | `score=0.996094` | pycoral `[det 0] score=0.9961` | match ✓ |
| detection box | `(90,17)-(160,98)` | pycoral `(74,14)-(131,80)` | ~20px 오차 (TFLite_Detection_PostProcess custom_options 미적용) |

> ★ "step 별로 byte-identical 까지 좁혀가고, 다음 step 에서 어긋나면 그 step 에 버그가 있다" 가 핵심 원칙.
> dump 만 하고 가설 없이 패턴 비교하는 "dump-analysis loop" 는 금지 (memory feedback_stop_dump_loop).

---

## 14. 알려진 limitation + future work

### Limitations

1. **box 좌표 ~20px 오차** (위 §9.2): TFLite_Detection_PostProcess 의 `custom_options` 의 실제 scale factors 가 표준 (10,10,5,5) 와 다를 수 있음. Netron 으로 GUI 에서 확인하거나, python `tflite` + `flatbuffers.flexbuffers` 로 디코딩.

2. **모델 1 종 hardcode**: 현재는 SSD MobileNet face 모델 quant/scale 값을 hpp 에 hardcode. 다른 모델 지원하려면 quant params 를 모델에서 동적으로 추출 (tflite 파일 파싱 추가 필요 — 현재 우리 LoadModel 은 DWN1 영역만 파싱하고 tflite Model 부분은 보지 않음).

3. **single-execution 만 지원**: `Relayout` 의 `execution_count_per_inference == 1` 만 동작. batch / multi-execution 모델은 미지원.

4. **chip cold start error handling 미흡**: 첫 inference 가 가끔 (drv-side timing) 실패. retry 로직 추가 필요.

### Future work (portfolio packaging 이후)

- TFLite_Detection_PostProcess custom_options 정확 추출 → box 좌표 100% 일치
- 다른 SSD 모델 (COCO 등) 지원
- streaming inference (video frame loop)
- benchmark: chip throughput, end-to-end latency
- 비교 baseline: pycoral 과 같은 image 로 latency 비교

---

## Appendix A. 메모리 (claude-code/memory) 관련 파일

이 프로젝트의 진행/판단 근거가 보존된 메모리:

```
MEMORY.md                              ← 인덱스
user_role.md                           ← portfolio 목적
feedback_communication_style.md        ← 존댓말 / 형님 호칭
feedback_dont_guess.md                 ← 검증 우선
feedback_stop_dump_loop.md             ← 가설 + 패치 + 빌드 + 결과 cycle
feedback_dump_format.md                ← CRC32 + HEAD/MID/TAIL 형식 규칙
project_apex_npu_driver.md             ← RESOLVED (chip 정상 확정)
project_eliminated_hypotheses.md       ← 38 개 죽인 가설 기록
project_va_layout.md                   ← VA 상수
project_postproc_milestone.md          ← 2026-05-18 standalone 완성
reference_codebases.md                 ← libedgetpu / coral.sys / pycoral 경로
reference_coral_sys_structure.md       ← 공식 Windows driver 분석
reference_ssd_outfeed_unpack.md        ← ff 00 80 80 의 정체 (★)
```

---

## Appendix B. 핵심 코드 한 줄로 정리된 ★ 위치

- **Relayout 알고리즘 본체**: `include/apex_postprocess.hpp:64` `apex_pp::Relayout`
- **uint8 → float dequant**: `include/apex_ssd_postprocess.hpp:56` `apex_pp::Dequantize`
- **SSD box decoding**: `include/apex_ssd_postprocess.hpp:68` `apex_pp::DecodeBoxes`
- **NMS**: `include/apex_ssd_postprocess.hpp:113` `apex_pp::NonMaxSuppression`
- **end-to-end pipeline**: `include/apex_ssd_postprocess.hpp:175` `apex_pp::RunSsdPostprocess`
- **모델 파서**: `include/apex_model_fb.hpp:66` `apex_fb::LoadModel`
- **LayerInfo + OutputLayout 추출**: `include/apex_model_fb.hpp:336~` (output_layers loop)
- **driver IOCTL 호출**: `infer_test_console/infer_test_console.cpp:447` `IOCTL_ALLOC_IO_BUFFERS`
- **chip 실행**: `infer_test_console/infer_test_console.cpp` `IOCTL_INFER_NEW` (Queue.c 의 handler 가 chip CSR 조작)
- **Relayout 호출 + bin 저장**: `infer_test_console.cpp:820~`
- **SSD postprocess 호출 + detection 저장**: `infer_test_console.cpp:870~`

---

작성: 2026-05-18
