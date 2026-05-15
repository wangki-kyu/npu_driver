#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#include <nmmintrin.h>   // _mm_crc32_u64 (SSE4.2 hardware CRC32)

#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>

#include "../include/util.hpp"
#include "../include/Public.h"
#include "../include/apex_model_fb.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// hex dump : 콘솔로
inline void DumpHex(const char* tag, const void* base,
    size_t offset, size_t len)
{
    const uint8_t* p = (const uint8_t*)base + offset;
    printf("=== [%s] base+0x%zx, %zu bytes ===\n", tag, offset, len);
    for (size_t i = 0; i < len; i += 16) {
        printf("  [0x%05zx]", offset + i);
        size_t row = (len - i >= 16) ? 16 : (len - i);
        for (size_t k = 0; k < row; ++k) printf(" %02x", p[i + k]);
        printf("  |");
        for (size_t k = 0; k < row; ++k) {
            uint8_t b = p[i + k];
            putchar((b >= 0x20 && b < 0x7f) ? b : '.');
        }
        printf("|\n");
    }
}

// Save raw output layer bytes to txt file (pycoral OUT-DUMP 와 byte 비교용)
// 형식: 한 줄당 16 byte hex + offset. Python 측 dump 와 diff 가능.
// 파일명에는 자동으로 timestamp 가 붙음: foo.txt → foo_YYYYMMDD_HHMMSS.txt
inline void SaveLayerToFile(const std::string& filename,
                            const void* base, size_t offset, size_t len)
{
    // timestamp 삽입
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    sprintf_s(ts, sizeof(ts), "_%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string stamped;
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) stamped = filename + ts;
    else                          stamped = filename.substr(0, dot) + ts + filename.substr(dot);

    FILE* f = nullptr;
    fopen_s(&f, stamped.c_str(), "w");
    if (!f) {
        printf("[save] FAIL: cannot open %s\n", stamped.c_str());
        return;
    }
    const uint8_t* p = (const uint8_t*)base + offset;
    fprintf(f, "=== %s size=%zu ===\n", stamped.c_str(), len);
    for (size_t i = 0; i < len; i += 16) {
        fprintf(f, "[0x%05zx]", i);
        size_t row = (len - i >= 16) ? 16 : (len - i);
        for (size_t k = 0; k < row; ++k) fprintf(f, " %02x", p[i + k]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("[save] wrote %s (%zu bytes)\n", stamped.c_str(), len);
}

// JPEG 로드 → aspect-preserving letterbox resize → 24bpp RGB 로 변환해 outBuf 에 채움.
//
// pycoral 의 common.set_resized_input 과 동일한 전처리:
//   1) scale = min(width/srcW, height/srcH)      ← 비율 유지 위해 작은 쪽
//   2) (newW, newH) = (srcW*scale, srcH*scale)   ← 비율 유지한 resize 대상
//   3) outBuf (width x height x 3) 를 0 으로 채우고
//   4) 좌상단 (0,0) 부터 newW x newH 영역에 resized image 배치
//   5) 우측 / 하단 padding 은 0 그대로
//
// 보간은 WICBitmapInterpolationModeFant — WIC 의 high-quality bilinear-like
// 알고리즘, PIL.Image.BILINEAR 와 byte 단위로 가장 유사한 결과를 냄.
static bool LoadJpegToRGB(const wchar_t* path, void* outBuf, UINT width, UINT height)
{
    HRESULT hr = S_OK;
    IWICImagingFactory*    pFactory   = nullptr;
    IWICBitmapDecoder*     pDecoder   = nullptr;
    IWICBitmapFrameDecode* pFrame     = nullptr;
    IWICBitmapScaler*      pScaler    = nullptr;
    IWICFormatConverter*   pConverter = nullptr;

    // 1) 출력 버퍼 전체를 0 으로 (letterbox padding).
    memset(outBuf, 0, (size_t)width * height * 3);

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void**)&pFactory);
    if (FAILED(hr)) { std::cerr << "CoCreateInstance failed: 0x" << std::hex << hr << std::endl; return false; }

    hr = pFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { std::cerr << "CreateDecoderFromFilename failed: 0x" << std::hex << hr << std::endl;
                      pFactory->Release(); return false; }

    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return false; }

    // 2) 원본 해상도 읽어서 aspect-preserving 새 크기 계산.
    UINT srcW = 0, srcH = 0;
    hr = pFrame->GetSize(&srcW, &srcH);
    if (FAILED(hr) || srcW == 0 || srcH == 0) {
        std::cerr << "GetSize failed or zero size" << std::endl;
        pFrame->Release(); pDecoder->Release(); pFactory->Release();
        return false;
    }
    double sx = (double)width  / (double)srcW;
    double sy = (double)height / (double)srcH;
    double scale = (sx < sy) ? sx : sy;
    UINT newW = (UINT)(srcW * scale);
    UINT newH = (UINT)(srcH * scale);
    if (newW == 0) newW = 1;
    if (newH == 0) newH = 1;
    if (newW > width)  newW = width;
    if (newH > height) newH = height;
    std::cout << "[image] source " << srcW << "x" << srcH
              << " -> letterbox " << newW << "x" << newH
              << " in " << width << "x" << height
              << " (scale=" << scale << ")" << std::endl;

    hr = pFactory->CreateBitmapScaler(&pScaler);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    // Fant = WIC 의 PIL.BILINEAR-equivalent.
    hr = pScaler->Initialize(pFrame, newW, newH, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat24bppRGB,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { pConverter->Release(); pScaler->Release(); pFrame->Release();
                      pDecoder->Release(); pFactory->Release(); return false; }

    // 3) resized image 를 임시 버퍼에 받고 → 좌상단에 letterbox 배치.
    UINT srcStride = newW * 3;
    UINT tempSize  = srcStride * newH;
    std::vector<BYTE> tempBuf(tempSize);
    hr = pConverter->CopyPixels(nullptr, srcStride, tempSize, tempBuf.data());

    pConverter->Release(); pScaler->Release(); pFrame->Release();
    pDecoder->Release();   pFactory->Release();

    if (FAILED(hr)) { std::cerr << "CopyPixels failed: 0x" << std::hex << hr << std::endl; return false; }

    // 4) outBuf 의 좌상단에 newW x newH 복사. 우측/하단 padding 은 0 그대로.
    UINT dstStride = width * 3;
    BYTE* dst = (BYTE*)outBuf;
    for (UINT r = 0; r < newH; r++) {
        memcpy(dst + r * dstStride, tempBuf.data() + r * srcStride, srcStride);
        // 같은 행의 (newW..width-1) 픽셀 = 0 (memset 으로 이미 채워둠).
    }
    // 행 (newH..height-1) = 0 (memset 으로 이미 채워둠).

    std::cout << "[image] loaded " << srcW << "x" << srcH
              << " -> " << newW << "x" << newH << " letterbox (Fant) in "
              << width << "x" << height << "x3 RGB" << std::endl;
    return true;
}

// device VA 설정
// ★ 각 slot 의 PTE 점유 범위가 겹치면 안 됨. simple PTE 는 1-level 이라 VA>>12 = PTE 인덱스.
//   slot size = N pages 이면 [VA, VA + N*0x1000) 전체가 다른 slot 점유 범위와 disjoint 해야 함.
//
//   ssd_mobilenet_v2_face 기준 worst case:
//     INPUT  = 320*320*3 = 307200B  →  75 pages → [VA_INPUT, VA_INPUT+0x4b000)
//     OUTPUT = 8136*2 (page-aligned) = 0x4000 → 4 pages
//     PARAM  = 6142720B  → 1500 pages → 0x5dc000
//     PARAM_BITSTREAM = 9808B → 3 pages
//     INFER_BITSTREAM = 199776B → 49 pages
//   여유 있게 1MB 단위로 슬롯 배치:
// ============================================================================
// [PARAM-DUMP] libedgetpu 와 동일 형식의 standard CRC32 dump 함수.
// libedgetpu/driver/package_registry.cc::ExecutableReference 생성자에 박힌
// dump 와 1:1 비교용. 같은 polynomial (0xEDB88320), 같은 init/finalize.
// ============================================================================
static uint32_t crc32_standard(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

static std::string hex32_line(const uint8_t* b, size_t cnt) {
    std::string s;
    char tmp[8];
    for (size_t i = 0; i < cnt; i++) {
        std::snprintf(tmp, sizeof(tmp), "%02x ", b[i]);
        s += tmp;
    }
    return s;
}

static void dump_buffer(const char* prefix, const char* label,
                        const void* data, size_t n) {
    if (data == nullptr || n == 0) {
        printf("[%s] user '%s' (empty)\n", prefix, label);
        return;
    }
    uint32_t crc = crc32_standard(data, n);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    printf("[%s] user label='%s' size=%zu CRC32=0x%08x\n", prefix, label, n, crc);
    printf("[%s]   HEAD: %s\n", prefix, hex32_line(p, 32).c_str());
    if (n >= 64) {
        printf("[%s]   MID:  %s\n", prefix, hex32_line(p + n / 2, 32).c_str());
    }
    if (n >= 32) {
        printf("[%s]   TAIL: %s\n", prefix, hex32_line(p + n - 32, 32).c_str());
    }
}

// 기존 호출자 (param dump) 호환 유지
static void dump_param(const char* label, const void* data, size_t n) {
    dump_buffer("PARAM-DUMP", label, data, n);
}

static const uint64_t VA_INPUT = 0x001000ULL;  // PTE[1..75]      input           ★ 0 금지
static const uint64_t VA_OUTPUT = 0x100000ULL;  // PTE[256..]     output          INPUT 너머
static const uint64_t VA_SCRATCH = 0x180000ULL;  // PTE[384..]    scratch         OUTPUT 너머
// PARAM VA — libedgetpu (driver/driver.cc:249 "Mapped params") 와 byte-identical
// 매칭. 가설: 칩 내부 SRAM 캐싱이 VA bits hash 로 bank addressing 한다면 같은
// 데이터라도 다른 VA 면 다른 bank 에 캐싱 → 모델이 expect 하는 위치와 mismatch.
//   exe1 PARAM_DATA (6144000B, 1500 pages): libedgetpu = 0x8000000000000000
//     → L1_idx=0..2 차지 (3개 region, L2 sub-table 시작이 ExtPool[0])
//   exe0 PARAM      (197440B,    49 pages): libedgetpu = 0x8000000000800000
//     → L1_idx=4 단일 region (exe1 과 충돌 없음)
static const uint64_t VA_EXE1_PARAM_DATA = 0x8000000000000000ULL;  // ext L1_idx=0..3
static const uint64_t VA_EXE0_PARAM_DATA = 0x8000000000800000ULL;  // ext L1_idx=4
static const uint64_t VA_PARAM_BITSTREAM = 0x840000ULL;  // PTE[2112..2114] exe1 bitstream
static const uint64_t VA_INFER_BITSTREAM = 0x900000ULL;  // PTE[2304..2352] exe0 bitstream
static const uint64_t VA_EXE0_BITSTREAM_PHASE1 = 0x800000ULL;  // libedgetpu pattern


int main(int argc, char** argv)
{
    printf("[BUILD] %s %s\n", __DATE__, __TIME__);
    // WIC 사용 위해 COM 초기화
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Model path: prefer argv[1], otherwise probe a few common candidates.
    std::vector<std::string> candidates;
    if (argc >= 2) candidates.push_back(argv[1]);
    candidates.push_back(".\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");
    candidates.push_back("..\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");
    candidates.push_back("..\\..\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");
    candidates.push_back("..\\..\\..\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");
    candidates.push_back("E:\\work\\project\\npu_driver\\model\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");

    apex_fb::ApexModelFb model;
    std::string used_path;
    for (const auto& p : candidates) {
        std::ifstream tf(p, std::ios::binary);
        if (!tf.is_open()) continue;
        tf.close();
        used_path = p;
        std::cout << "[main] trying model path: " << p << std::endl;
        model = apex_fb::LoadModel(p);
        if (!model.bitstream.empty()) break;
    }
    if (model.bitstream.empty()) {
        std::cout << "[main] FAIL: could not load ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite from any candidate path" << std::endl;
        return 1;
    }
    std::cout << "[main] model loaded from: " << used_path << std::endl;

    if (model.input_layers.empty() || model.output_layers.empty()) {
        std::cout << "[main] FAIL: model has no input or output layers" << std::endl;
        return 1;
    }

    // ========================================================================
    // [PARAM-DUMP] LoadModel 직후, memcpy 이전 — libedgetpu 와 직접 비교용.
    // libedgetpu/driver/package_registry.cc::ExecutableReference 생성자가
    // exe0/exe1 각각에 대해 같은 형식 dump 를 stderr 로 찍는다. 두 출력의
    // size + CRC32 가 일치하면 본인 apex_model_fb.hpp::LoadModel 의 schema
    // 파싱이 libedgetpu 와 byte-perfect 호환.
    // ========================================================================
    dump_param("exe0_main_parameters",     model.exe0_parameters.data(), model.exe0_parameters.size());
    dump_param("exe1_param_caching_data", model.parameters.data(),      model.parameters.size());

    DWORD bytesReturned;
    bool isExistParam = !model.param_bitstream.empty() && !model.parameters.empty();

    // -------------------------------------------------------------------------
    // size check
    // -------------------------------------------------------------------------
    const size_t INPUT_SIZE = model.input_layers[0].size_bytes;
    const size_t OUTPUT_SIZE = model.total_output_size_bytes;
    const size_t SCRATCH_SIZE = model.scratch_size_bytes;
    const size_t EX0_BITSTREAM_SIZE = model.bitstream.size();
    const size_t PARAM_SIZE = model.parameters.size();
    const size_t PARAM_BITSTREAM_SIZE = model.param_bitstream.size();
    const size_t EXE0_PARAM_SIZE = model.exe0_parameters.size();
    
    
   // -------------------------------------------------------------------------
   // Device handle
   // -------------------------------------------------------------------------
    HANDLE handle = FindPicoDriverDevice(GUID_DEVINTERFACE_npudriver);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        std::cout << "[main] FAIL: device handle: " << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "[main] device handle OK" << std::endl;

    // -------------------------------------------------------------------------
    // contiguous allocate & pte mapping ioctl request
    // -------------------------------------------------------------------------
    // User buffer pointers -- single cleanup path frees them all.
    void* pParamData = nullptr; // exe1.parameters() data
    void* pParamBitstream = nullptr; // exe1 bitstream
    void* pExe0ParamData = nullptr; // exe0 parameters data
    void* pExe0Phase1Bs = nullptr; // exe0 bitstream copy mapped during Phase1
    void* pInferBitstream = nullptr; // exe0 bitstream (patched) for INFER mapping
    void* pInputBuf = nullptr;
    void* pOutputBuf = nullptr;
    void* pScratchBuf = nullptr;

    IOCTL_ALLOC_IO_BUFFERS_IN allocIn = {};
    IOCTL_ALLOC_IO_BUFFERS_OUT allocOut = {};
    allocIn.InputSize = INPUT_SIZE;
    allocIn.InputDeviceVA = VA_INPUT;
    allocIn.OutputSize = OUTPUT_SIZE;
    allocIn.OutputDeviceVA = VA_OUTPUT;
    allocIn.ScratchSize = SCRATCH_SIZE;
    allocIn.ScratchDeviceVA = (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0;
    allocIn.Exe0BitstreamSize = EX0_BITSTREAM_SIZE;
    allocIn.Exe0BitstreamDeviceVA = VA_INFER_BITSTREAM;
    
    if (isExistParam) {
        allocIn.ParamDataSize = PARAM_SIZE;
        allocIn.ParamDataDeviceVA = VA_EXE1_PARAM_DATA; // 6.14MB exe1 용
        allocIn.Exe1BitstreamSize = PARAM_BITSTREAM_SIZE;
        allocIn.Exe1BitstreamDeviceVA = VA_PARAM_BITSTREAM;
        allocIn.Exe0ParamSize = EXE0_PARAM_SIZE;
        allocIn.Exe0ParamDeviceVA = VA_EXE0_PARAM_DATA;
    }
    else {
        allocIn.ParamDataSize = 0;
        allocIn.ParamDataDeviceVA = 0;
        allocIn.Exe1BitstreamSize = 0;
        allocIn.Exe1BitstreamDeviceVA = 0;
        allocIn.Exe0ParamSize = 0;
        allocIn.Exe0ParamDeviceVA = 0;
    }

    if (!DeviceIoControl(handle, IOCTL_ALLOC_IO_BUFFERS, &allocIn, sizeof(allocIn), &allocOut, sizeof(allocOut), &bytesReturned, nullptr)) {
        std::cout << "[main] FAIL: IOCTL_ALLOC_IO_BUFFERS: " << GetLastError() << std::endl;
        goto cleanup;
    }

    pInputBuf = (void*)allocOut.InputUserVA;
    pOutputBuf = (void*)allocOut.OutputUserVA;
    pScratchBuf = (SCRATCH_SIZE > 0) ? (void*)allocOut.ScratchUserVA : nullptr;
    pInferBitstream = (void*)allocOut.Exe0BitStreamUserVA;

    if (isExistParam) {
        pParamData = (void*)allocOut.ParamDataUserVA;
        pParamBitstream = (void*)allocOut.Exe1BitstreamUserVA;
        pExe0ParamData = (void*)allocOut.Exe0ParamUserVA;

        memcpy(pParamData, model.parameters.data(), model.parameters.size());               // <-- exe1 param
        memcpy(pExe0ParamData, model.exe0_parameters.data(), model.exe0_parameters.size()); // <-- exezhem 0 param

        // 이게 뭐하는건지 정확히 모르겠음. 
        /*MemoryBarrier();
        _mm_sfence();*/

        // memcpy 검증: src(model.parameters) 와 dst(UserVA → 결국 chip 이 DMA 로 읽을 PA) 가 같은지.
        // driver 쪽에서 같은 시점에 KVA 기준으로 dump 해서 3-way 비교하면 매핑 깨짐 즉시 보임.
        //
        // === Phase-1 진단: 6.14MB memcpy integrity 전체 검증 ===
        // 첫 32바이트만 비교하면 head 만 일치하고 tail 이 잘려도 못 잡음.
        // head + mid + tail 3 지점 + 전체 CRC32 로 완전 검증.
        auto crc32_buf = [](const void* p, size_t n) -> uint64_t {
            const uint8_t* b = (const uint8_t*)p;
            uint64_t crc = ~0ULL;
            // 8-byte chunk 빠르게
            size_t n8 = n / 8;
            for (size_t i = 0; i < n8; i++) {
                uint64_t v;
                std::memcpy(&v, b + i * 8, 8);
                crc = _mm_crc32_u64(crc, v);
            }
            // tail 1-byte
            for (size_t i = n8 * 8; i < n; i++) {
                crc = _mm_crc32_u8((uint32_t)crc, b[i]);
            }
            return ~crc;
        };

        // ---------- exe1 parameters (6.14MB) ----------
        std::cout << "\n[param-verify] exe1 parameters (size=" << model.parameters.size() << ")" << std::endl;
        {
            size_t sz = model.parameters.size();
            const uint8_t* src = model.parameters.data();
            const uint8_t* dst = (const uint8_t*)pParamData;

            DumpHex("  src HEAD",  src, 0,           32);
            DumpHex("  dst HEAD",  dst, 0,           32);
            DumpHex("  src MID",   src, sz / 2,      32);
            DumpHex("  dst MID",   dst, sz / 2,      32);
            DumpHex("  src TAIL",  src, sz - 32,     32);
            DumpHex("  dst TAIL",  dst, sz - 32,     32);

            uint64_t crc_src = crc32_buf(src, sz);
            uint64_t crc_dst = crc32_buf(dst, sz);
            int head_eq = (std::memcmp(src, dst, 32) == 0);
            int mid_eq  = (std::memcmp(src + sz / 2, dst + sz / 2, 32) == 0);
            int tail_eq = (std::memcmp(src + sz - 32, dst + sz - 32, 32) == 0);
            int crc_eq  = (crc_src == crc_dst);
            printf("[param-verify] exe1: HEAD=%s MID=%s TAIL=%s  CRC32 src=0x%016llx dst=0x%016llx %s\n",
                head_eq ? "OK" : "DIFF",
                mid_eq  ? "OK" : "DIFF",
                tail_eq ? "OK" : "DIFF",
                (unsigned long long)crc_src,
                (unsigned long long)crc_dst,
                crc_eq ? "MATCH" : "MISMATCH!!!");
            if (!crc_eq) {
                // 첫 불일치 byte offset 찾기 (이진 탐색 비슷하게)
                size_t first_diff = (size_t)-1;
                for (size_t i = 0; i < sz; i++) {
                    if (src[i] != dst[i]) { first_diff = i; break; }
                }
                printf("[param-verify] exe1: first byte diff at offset 0x%zx / 0x%zx "
                       "(%.2f%% into buffer)  src=0x%02x dst=0x%02x\n",
                       first_diff, sz, 100.0 * first_diff / sz,
                       src[first_diff], dst[first_diff]);
            }
        }

        // ---------- exe0 parameters (192KB) ----------
        std::cout << "[param-verify] exe0 parameters (size=" << model.exe0_parameters.size() << ")" << std::endl;
        {
            size_t sz = model.exe0_parameters.size();
            const uint8_t* src = model.exe0_parameters.data();
            const uint8_t* dst = (const uint8_t*)pExe0ParamData;

            DumpHex("  src HEAD",  src, 0,           32);
            DumpHex("  dst HEAD",  dst, 0,           32);
            DumpHex("  src TAIL",  src, sz - 32,     32);
            DumpHex("  dst TAIL",  dst, sz - 32,     32);

            uint64_t crc_src = crc32_buf(src, sz);
            uint64_t crc_dst = crc32_buf(dst, sz);
            int head_eq = (std::memcmp(src, dst, 32) == 0);
            int tail_eq = (std::memcmp(src + sz - 32, dst + sz - 32, 32) == 0);
            int crc_eq  = (crc_src == crc_dst);
            printf("[param-verify] exe0: HEAD=%s TAIL=%s  CRC32 src=0x%016llx dst=0x%016llx %s\n",
                head_eq ? "OK" : "DIFF",
                tail_eq ? "OK" : "DIFF",
                (unsigned long long)crc_src,
                (unsigned long long)crc_dst,
                crc_eq ? "MATCH" : "MISMATCH!!!");
            if (!crc_eq) {
                size_t first_diff = (size_t)-1;
                for (size_t i = 0; i < sz; i++) {
                    if (src[i] != dst[i]) { first_diff = i; break; }
                }
                printf("[param-verify] exe0: first byte diff at offset 0x%zx / 0x%zx "
                       "(%.2f%% into buffer)  src=0x%02x dst=0x%02x\n",
                       first_diff, sz, 100.0 * first_diff / sz,
                       src[first_diff], dst[first_diff]);
            }
        }

        //size_t sz = model.parameters.size();        // 6.14MB
        //size_t q = sz / 4;
        // 4번 빌드/실행, 한 번에 한 줄만 활성화:
        //memset((char*)pParamData + 0 * q, 0x00, q);   // 1st quarter
        //memset((char*)pParamData + 1 * q, 0x00, q);   // 2nd quarter
        //memset((char*)pParamData + 2 * q, 0x00, q);   // 3rd quarter
        //memset((char*)pParamData + 3 * q, 0x00, q);   // 4th quarter (last)

        //size_t sz0 = model.exe0_parameters.size();   // 196KB
        //size_t q0 = sz0 / 4;
        //memset((char*)pExe0ParamData + 0 * q0, 0x00, q0);  // 1st quarter of exe0 보조
         //memset((char*)pExe0ParamData + 1*q0, 0x00, q0);  // 2번: 2nd quarter
         //memset((char*)pExe0ParamData + 2*q0, 0x00, q0);  // 3번: 3rd quarter
         //memset((char*)pExe0ParamData + 3*q0, 0x00, q0);  // 4번: 4th quarter

        // === DIAGNOSTIC ===
        //memset(pParamData, 0x00, model.parameters.size());          // weight 를 전부 0 으로
        //memset(pExe0ParamData, 0x00, model.exe0_parameters.size()); // exe0 보조도 0
        // ==================
    }

    // -------------------------------------------------------------------------
    // param caching
    // -------------------------------------------------------------------------
    // 1. patch exe1 bitstream with param va
    // 2. ioctl instruction descriptor 
    if (isExistParam) {
        if (pParamData == NULL || pParamBitstream == NULL) {
            std::cout << "param is not working here" << std::endl;
            goto cleanup;
        }
    }

    {
        auto dump = [](const char* tag, const uint8_t* p, size_t n) {
            std::cout << tag;
            for (size_t i = 0; i < n; ++i) printf(" %02x", p[i]);
            std::cout << "\n";
            };


        std::cout << "\n--- patch device VAs ---" << std::endl;
        apex_fb::PatchParamBitstreamVAs(model, VA_EXE1_PARAM_DATA);
        dump("[SW after patch  byte 0x40..0x5f]",
            model.param_bitstream.data() + 0x40, 0x20);

        apex_fb::DumpParamPatchedVAs(model);
        apex_fb::DumpParamPatchRawValues(model);
        std::cout << "Exe1BitstreamUserVA: " << allocOut.Exe0BitStreamUserVA << std::endl;
        memcpy((void*)allocOut.Exe1BitstreamUserVA, model.param_bitstream.data(), model.param_bitstream.size());

        dump("[CHIP after memcpy byte 0x40..0x5f]",
            (uint8_t*)allocOut.Exe1BitstreamUserVA + 0x40, 0x20);

        apex_fb::PatchVAs(model, VA_INPUT, VA_OUTPUT, VA_EXE0_PARAM_DATA,
                          (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0);
        apex_fb::DumpPatchedVAs(model);
        memcpy((void*)allocOut.Exe0BitStreamUserVA, model.bitstream.data(), model.bitstream.size());
    }

    // -------------------------------------------------------------------------
    // patched bitstream / param data 를 contiguous slot 으로 memcpy.
    // chip 은 std::vector 가 아니라 slot 의 PA 에서 DMA 하므로 한 번 복사 필요.
    // -------------------------------------------------------------------------
    memcpy(pInferBitstream, model.bitstream.data(), model.bitstream.size());
    if (isExistParam) {
        memcpy(pParamBitstream, model.param_bitstream.data(), model.param_bitstream.size());
    }

    apex_fb::DumpChipVisibleBitstream("exe0", pInferBitstream, model.patches);
    if (isExistParam)
        apex_fb::DumpChipVisibleBitstream("exe1", pParamBitstream, model.param_patches);

    // ========================================================================
    // [PATCH-DUMP] libedgetpu 와 비교용 한 줄 형식.
    // libedgetpu 측 매칭 출력은 instruction_buffers.cc::LinkInstructionBuffers.
    // 각 patch site 의 (desc, position, name, byte-offset, 32-bit value) 를
    // 한 줄씩 dump → grep 으로 양쪽 line 추출해서 diff 하면 patch encoding
    // 차이가 정확히 어떤 자리에서 발생하는지 보임.
    // ========================================================================
    if (isExistParam) {
        apex_fb::DumpPatchValuesForDiff("exe1", pParamBitstream, model.param_patches);
    }
    apex_fb::DumpPatchValuesForDiff("exe0", pInferBitstream, model.patches);

    // -------------------------------------------------------------------------
    // input image 로드 → input slot 으로 직접 채움.
    // INPUT_SIZE = side*side*3 (24bpp RGB) 가정. square 모델만 지원.
    //
    // USE_LIBEDGETPU_INPUT toggle:
    //   1 = libedgetpu 가 dump 한 finalized host_input 을 verbatim 로드
    //       (preprocessing 우회 — input divergence 격리 테스트용).
    //       libedgetpu/driver/single_tpu_request.cc 의 [INPUT-CRC] 블록이
    //       inference 직전 C:\temp\libe_input_raw.bin 으로 307200B dump.
    //   0 = 기존 JPG 로드 + Fant resize + letterbox.
    // -------------------------------------------------------------------------
    #define USE_LIBEDGETPU_INPUT 1
    {
        UINT imgSide = static_cast<UINT>(std::sqrt(static_cast<double>(INPUT_SIZE) / 3.0));
        if (imgSide * imgSide * 3 != INPUT_SIZE) {
            std::cout << "[main] WARNING: input size " << INPUT_SIZE
                      << " is not square*3 — falling back to imgSide=" << imgSide << std::endl;
        }

#if USE_LIBEDGETPU_INPUT
        {
            const char* libe_input_path = "C:\\temp\\libe_input_raw.bin";
            FILE* f = nullptr;
            fopen_s(&f, libe_input_path, "rb");
            if (f) {
                size_t r = fread(pInputBuf, 1, INPUT_SIZE, f);
                fclose(f);
                std::cout << "[main] USE_LIBEDGETPU_INPUT=1 — loaded " << r << "/"
                          << INPUT_SIZE << " bytes from " << libe_input_path << std::endl;
                if (r != INPUT_SIZE) {
                    std::cout << "[main] FAIL: libe input file size mismatch — expected "
                              << INPUT_SIZE << " B" << std::endl;
                    goto cleanup;
                }
            } else {
                std::cout << "[main] FAIL: " << libe_input_path
                          << " not found — run libedgetpu first to generate it" << std::endl;
                goto cleanup;
            }
        }
#else
        std::cout << "[main] loading image (target " << imgSide << "x" << imgSide << ")" << std::endl;
        if (!LoadJpegToRGB(L".\\assets\\karina.jpg", pInputBuf, imgSide, imgSide)) {
            std::cout << "[main] FAIL: image load" << std::endl;
            goto cleanup;
        }
#endif
    }

    DumpHex("after-PC slot0 head", (const void*)allocOut.InputUserVA, 0x0000, 0x100);
    DumpHex("after-PC slot0 0xe000", (const void*)allocOut.InputUserVA, 0xe000, 0x200);
    DumpHex("after-PC slot0 0xf000", (const void*)allocOut.InputUserVA, 0xf000, 0x100);

    // ========================================================================
    // [INPUT-DUMP] pycoral run_infer.py 의 [INPUT-DUMP] 와 byte-by-byte 비교용.
    // 동일 형식 (CRC32 + HEAD + MID + TAIL). 양쪽 같으면 preprocessing 일치,
    // 다르면 보간 알고리즘 (Cubic vs BILINEAR) 차이가 단독 원인.
    // INPUT_SIZE = 320 * 320 * 3 = 307200 bytes (24bpp RGB)
    // ========================================================================
    dump_buffer("INPUT-DUMP", "normalized_input_image_tensor",
                (const void*)allocOut.InputUserVA, INPUT_SIZE);

    // output/scratch 를 sentinel 로 채움 — chip 이 실제로 덮은 byte 와 안 덮은 byte 구분.
    // 0xCC/0xAA 는 chip outfeed 결과로 자연스럽게 나오기 어려운 값. 잔존 시 "chip 안 씀" 신호.
    memset(pOutputBuf, 0xCC, OUTPUT_SIZE);
    if (pScratchBuf) memset(pScratchBuf, 0xAA, SCRATCH_SIZE);

    // -------------------------------------------------------------------------
    // infer new
    // -------------------------------------------------------------------------
    {
        IOCTL_INFER_INFO ii = {};
        ii.InputImageAddr = (UINT64)pInputBuf;
        ii.InputImageSize = INPUT_SIZE;
        ii.OutputBufferAddr = (UINT64)pOutputBuf;
        ii.OutputBufferSize = OUTPUT_SIZE;
        ii.InputDeviceVA = VA_INPUT;
        ii.OutputDeviceVA = VA_OUTPUT;
        ii.BitstreamDeviceVA = VA_INFER_BITSTREAM;
        ii.BitstreamSize = model.bitstream.size();
        ii.ScratchAddr = (UINT64)pScratchBuf;
        ii.ScratchSize = SCRATCH_SIZE;
        ii.ScratchDeviceVA = (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0;

        BOOL ok = DeviceIoControl(handle, IOCTL_INFER_NEW, &ii, sizeof(ii),
            nullptr, 0, &bytesReturned, nullptr);
        if (ok) {
            std::cout << "[main] INFER OK" << std::endl;
        }
        else {
            std::cout << "[main] INFER failed: " << GetLastError()
                << " -- output dump still printed below for debugging" << std::endl;
        }

        PUCHAR p = (PUCHAR)pOutputBuf + 0x2000;
        size_t same_count = 0, n_anchor = 8136 / 4;  // 2034
        for (size_t i = 0; i < n_anchor; i++) {
            if (p[i * 4] == 0xff && p[i * 4 + 1] == 0x00 && p[i * 4 + 2] == 0x80 && p[i * 4 + 3] == 0x80) same_count++;
        }
        printf("[uniform] convert_scores: %zu / %zu anchors match 'ff 00 80 80'\n",
            same_count, n_anchor);

        // === diff-anchor: ff 00 80 80 와 다른 anchor 의 byte 패턴 보기 ===
        {
            size_t shown = 0;
            for (size_t i = 0; i < n_anchor && shown < 20; i++) {
                if (!(p[i*4]==0xff && p[i*4+1]==0x00 && p[i*4+2]==0x80 && p[i*4+3]==0x80)) {
                    printf("[diff-anchor] idx=%zu : %02x %02x %02x %02x\n",
                           i, p[i*4], p[i*4+1], p[i*4+2], p[i*4+3]);
                    shown++;
                }
            }
            if (shown == 0) {
                printf("[diff-anchor] (none — all 2034 anchors are 'ff 00 80 80')\n");
            }
        }
    }





    // -------------------------------------------------------------------------
    // outfeed inspect — chip 이 OUTPUT 영역에 실제로 무엇을 썼는지 byte 단위 진단.
    //   1) coverage  : sentinel(0xCC) 잔존 byte 카운트 + 영역별 written/untouched
    //   2) per-layer : 각 output_layer 의 head/tail hex dump.
    //                  Python pycoral 의 [OUT-DUMP] (raw TPU tensor) 와 byte 단위
    //                  비교 가능한 포맷. (chip 출력은 quant uint8/int8 — float 해석 X)
    // -------------------------------------------------------------------------
    {
        PUCHAR output = (PUCHAR)pOutputBuf;

        // (1) coverage summary
        size_t still_cc = 0, nonZero = 0;
        for (size_t i = 0; i < OUTPUT_SIZE; i++) {
            if (output[i] == 0xCC) still_cc++;
            if (output[i] != 0)    nonZero++;
        }
        std::cout << "[outfeed] OUTPUT_SIZE=" << OUTPUT_SIZE
                  << "  written~=" << (OUTPUT_SIZE - still_cc)
                  << "  still-0xCC=" << still_cc
                  << "  non-zero=" << nonZero << std::endl;

        // (2) per-layer head + tail dump (Python pycoral [OUT-DUMP] 와 직접 비교)
        size_t off = 0;
        for (size_t li = 0; li < model.output_layers.size(); li++) {
            const auto& layer = model.output_layers[li];
            size_t aligned = apex_fb::PageAlignUp(layer.size_bytes);

            // 영역별 sentinel 잔존 카운트 (chip 이 이 layer 까지 outfeed 했는지)
            size_t layer_cc = 0;
            for (size_t i = 0; i < aligned && off + i < OUTPUT_SIZE; i++)
                if (output[off + i] == 0xCC) layer_cc++;

            std::cout << "\n[outfeed] layer[" << li << "] '" << layer.name
                      << "' off=0x" << std::hex << off
                      << " logical=" << std::dec << layer.size_bytes
                      << " aligned=0x" << std::hex << aligned << std::dec
                      << " still-0xCC=" << layer_cc << "/" << aligned << std::endl;

            // head: 첫 256 byte (Python OUT-DUMP 와 동일 양)
            size_t headN = (std::min<size_t>)(layer.size_bytes, (size_t)256);
            DumpHex("  head", output, off, headN);

            // tail: 마지막 64 byte — chip 이 layer 끝까지 outfeed 했는지 확인용
            if (layer.size_bytes > 256) {
                size_t tailOff = off + layer.size_bytes - 64;
                DumpHex("  tail", output, tailOff, 64);
            }

            // 전체 byte 를 txt 파일로 저장 — pycoral OUT-DUMP 와 diff 용
            //SaveLayerToFile(layer.name + ".txt", output, off, layer.size_bytes);

            off += aligned;
        }
    }

cleanup:
    if (handle && handle != INVALID_HANDLE_VALUE) {
        DWORD br = 0;
        DeviceIoControl(handle, IOCTL_FREE_IO_BUFFERS, nullptr, 0, nullptr, 0, &br, nullptr);
        CloseHandle(handle);
    }
    CoUninitialize();
    return 0;
}

