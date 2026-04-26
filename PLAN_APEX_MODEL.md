# apex_model.hpp 구현 계획

## 목적

`edgetpu.tflite` 파일에서 명령어 비트스트림을 추출하고, 입력/출력 device VA를
bitstream 내 MOVI 명령어 immediate 필드에 패치하는 헬퍼 헤더.
드라이버에서 FlatBuffer 파싱은 부적절하므로 userspace hpp로 분리.

---

## 의존성

| 파일 | 용도 |
|------|------|
| `include/flatbuffers/flatbuffers.h` | FlatBuffers v25.12.19 헤더 (이미 있음) |
| `include/executable_generated.h` | flatc 생성 파싱 코드 (이미 있음) |

---

## FlatBuffer 파싱 경로

edgetpu.tflite는 이중 중첩 직렬화 구조:

```
edgetpu.tflite (raw bytes)
 └─ GetPackage(buf)                                        → Package
      └─ serialized_multi_executable()->data()             → 중첩 FlatBuffer bytes
           └─ GetRoot<MultiExecutable>(data)               → MultiExecutable
                └─ serialized_executables()->Get(0)->data() → 또다시 bytes
                     └─ GetRoot<Executable>(data)          → Executable
                          └─ instruction_bitstreams()->Get(0) → InstructionBitstream
                               ├─ bitstream()              → 명령어 raw bytes (패치 대상)
                               └─ field_offsets()          → 패치 위치 목록
```

`GetRoot<T>()` 를 두 번 중첩 호출해야 함 (Package → MultiExecutable, String → Executable).

---

## FieldOffset 패치 방식

```
FieldOffset
  ├─ meta()->desc()      : Description enum
  │    ├─ BASE_ADDRESS_INPUT_ACTIVATION  (1) → 입력 이미지 device VA
  │    └─ BASE_ADDRESS_OUTPUT_ACTIVATION (0) → 출력 버퍼 device VA
  ├─ meta()->position()  : Position enum
  │    ├─ LOWER_32BIT (0) → va & 0xFFFFFFFF
  │    └─ UPPER_32BIT (1) → va >> 32
  └─ offset_bit()        → bitstream 내 byte 위치 = offset_bit / 8
```

패치 공식:
```cpp
uint64_t va  = (desc == INPUT) ? input_device_va : output_device_va;
uint32_t val = (position == LOWER) ? (uint32_t)(va & 0xFFFFFFFF)
                                   : (uint32_t)(va >> 32);
memcpy(bitstream.data() + offset_bit / 8, &val, 4);
```

---

## device VA 계산 방식 (단순 연속 PTE)

Extended VA 대신, 모델 bitstream 뒤에 연속으로 배치:

```
PTE[0  .. M-1]     : bitstream  (device VA = 0x0)
PTE[M  .. M+I-1]   : 입력 이미지 (device VA = M * PAGE_SIZE)
PTE[M+I.. M+I+O-1] : 출력 버퍼  (device VA = (M+I) * PAGE_SIZE)
```

```
input_va  = PAGE_ALIGN_UP(bitstream_size)
output_va = input_va + PAGE_ALIGN_UP(input_image_size)
```

총 PTE = M + I + O ≤ 8192 (6.5MB 모델=1628, 입력 ~24페이지 → 여유 충분)

---

## API

```cpp
namespace apex {

struct FieldPatch {
    int32_t  offset_bit;
    platforms::darwinn::Description desc;
    platforms::darwinn::Position    position;
};

struct ApexModel {
    std::vector<uint8_t>    bitstream;  // mutable copy, PatchVAs로 수정
    std::vector<FieldPatch> patches;    // 패치 위치 목록
};

// tflite 파일 로드 → bitstream 추출 + patches 수집
ApexModel LoadModel(const std::string& path);

// bitstream in-place 패치 (IOCTL_MAP_BUFFER 전에 호출)
void PatchVAs(ApexModel& model, uint64_t input_device_va, uint64_t output_device_va);

// 유틸리티
inline uint64_t PageAlignUp(uint64_t n) { return (n + 4095ULL) & ~4095ULL; }

} // namespace apex
```

---

## LoadModel 구현

```cpp
ApexModel LoadModel(const std::string& path) {
    // 1. 파일 로드
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = (size_t)f.tellg();
    std::vector<uint8_t> raw(sz);
    f.seekg(0);
    f.read((char*)raw.data(), sz);

    // 2. 이중 파싱
    const auto* pkg = platforms::darwinn::GetPackage(raw.data());
    if (!pkg || !pkg->serialized_multi_executable()) return {};

    const auto* multi = flatbuffers::GetRoot<platforms::darwinn::MultiExecutable>(
                            pkg->serialized_multi_executable()->data());
    if (!multi || !multi->serialized_executables() ||
        multi->serialized_executables()->size() == 0) return {};

    const auto* ser = multi->serialized_executables()->Get(0);
    const auto* exec = flatbuffers::GetRoot<platforms::darwinn::Executable>(ser->data());
    if (!exec || !exec->instruction_bitstreams() ||
        exec->instruction_bitstreams()->size() == 0) return {};

    const auto* ibs = exec->instruction_bitstreams()->Get(0);
    if (!ibs || !ibs->bitstream()) return {};

    // 3. bitstream 복사
    ApexModel model;
    model.bitstream.assign(ibs->bitstream()->begin(), ibs->bitstream()->end());

    // 4. FieldOffset 수집
    if (ibs->field_offsets()) {
        for (const auto* fo : *ibs->field_offsets()) {
            if (!fo || !fo->meta()) continue;
            FieldPatch p;
            p.offset_bit = fo->offset_bit();
            p.desc       = fo->meta()->desc();
            p.position   = fo->meta()->position();
            // 범위 검사
            if (p.offset_bit / 8 + 4 <= (int32_t)model.bitstream.size())
                model.patches.push_back(p);
        }
    }
    return model;
}
```

---

## PatchVAs 구현

```cpp
void PatchVAs(ApexModel& model, uint64_t input_va, uint64_t output_va) {
    using namespace platforms::darwinn;
    for (const auto& p : model.patches) {
        if (p.desc != Description_BASE_ADDRESS_INPUT_ACTIVATION &&
            p.desc != Description_BASE_ADDRESS_OUTPUT_ACTIVATION) continue;

        uint64_t va  = (p.desc == Description_BASE_ADDRESS_INPUT_ACTIVATION)
                       ? input_va : output_va;
        uint32_t val = (p.position == Position_LOWER_32BIT)
                       ? (uint32_t)(va & 0xFFFFFFFF)
                       : (uint32_t)(va >> 32);
        std::memcpy(model.bitstream.data() + p.offset_bit / 8, &val, sizeof(uint32_t));
    }
}
```

---

## test_console 사용 예시

```cpp
#include "../include/apex_model.hpp"

// 1. 모델 로드 + bitstream 추출
apex::ApexModel model = apex::LoadModel(".\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");
if (model.bitstream.empty()) { /* 에러 처리 */ }

// 2. device VA 계산
const size_t INPUT_SIZE  = 320 * 320 * 3;        // 96000 bytes
const size_t OUTPUT_SIZE = 4 * 1917 * 4;          // 추정값 (SSD 출력)
uint64_t input_va  = apex::PageAlignUp(model.bitstream.size());
uint64_t output_va = input_va + apex::PageAlignUp(INPUT_SIZE);

// 3. bitstream 패치
apex::PatchVAs(model, input_va, output_va);

// 4. 패치된 bitstream을 IOCTL_MAP_BUFFER로 드라이버에 등록
MAP_BUFFER_INPUT mapInput;
mapInput.UserAddress = (UINT64)model.bitstream.data();
mapInput.Size        = model.bitstream.size();
DeviceIoControl(handle, IOCTL_MAP_BUFFER, &mapInput, sizeof(mapInput), ...);

// 5. 입력/출력 버퍼 할당
void* pImageBuf  = AllocateAlignedMemory(INPUT_SIZE,  4096);
void* pOutputBuf = AllocateAlignedMemory(OUTPUT_SIZE, 4096);
// ... 이미지 데이터 채우기 ...

// 6. IOCTL_INFER 호출
IOCTL_INFER_INFO inferInput = {};
inferInput.InputImageAddr    = (UINT64)pImageBuf;
inferInput.InputImageSize    = INPUT_SIZE;
inferInput.OutputBufferAddr  = (UINT64)pOutputBuf;
inferInput.OutputBufferSize  = OUTPUT_SIZE;
inferInput.InputDeviceVA     = input_va;
inferInput.OutputDeviceVA    = output_va;
inferInput.BitstreamDeviceVA = 0;
inferInput.BitstreamSize     = model.bitstream.size();

BOOL ok = DeviceIoControl(handle, IOCTL_INFER,
                          &inferInput, sizeof(inferInput),
                          nullptr, 0, &bytesReturned, nullptr);
```

---

## 주의사항

- `model.bitstream`은 `std::vector`이므로 heap 할당됨 → 페이지 정렬 보장 없음  
  → `IOCTL_MAP_BUFFER` 전에 `_aligned_malloc(bitstream.size(), 4096)` 후 복사 필요
- `PARAMETER` / `SCRATCH` desc는 패치하지 않음 (모델 가중치 주소는 별도 처리)
- 출력 크기는 `Executable::output_layers()` 로 정확히 구할 수 있음 (선택적 구현)
