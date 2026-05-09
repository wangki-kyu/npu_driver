#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>

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

// JPEG 로드 → resize → 24bpp RGB 로 변환해 outBuf 에 채움. (npu_test_console.cpp 와 동일)
static bool LoadJpegToRGB(const wchar_t* path, void* outBuf, UINT width, UINT height)
{
    HRESULT hr = S_OK;
    IWICImagingFactory*    pFactory   = nullptr;
    IWICBitmapDecoder*     pDecoder   = nullptr;
    IWICBitmapFrameDecode* pFrame     = nullptr;
    IWICBitmapScaler*      pScaler    = nullptr;
    IWICFormatConverter*   pConverter = nullptr;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void**)&pFactory);
    if (FAILED(hr)) { std::cerr << "CoCreateInstance failed: 0x" << std::hex << hr << std::endl; return false; }

    hr = pFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { std::cerr << "CreateDecoderFromFilename failed: 0x" << std::hex << hr << std::endl;
                      pFactory->Release(); return false; }

    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return false; }

    hr = pFactory->CreateBitmapScaler(&pScaler);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pScaler->Initialize(pFrame, width, height, WICBitmapInterpolationModeCubic);
    if (FAILED(hr)) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat24bppRGB,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { pConverter->Release(); pScaler->Release(); pFrame->Release();
                      pDecoder->Release(); pFactory->Release(); return false; }

    UINT stride     = width * 3;        // 24bpp = 3 bytes/pixel
    UINT bufferSize = stride * height;
    hr = pConverter->CopyPixels(nullptr, stride, bufferSize, (BYTE*)outBuf);

    pConverter->Release(); pScaler->Release(); pFrame->Release();
    pDecoder->Release();   pFactory->Release();

    if (FAILED(hr)) { std::cerr << "CopyPixels failed: 0x" << std::hex << hr << std::endl; return false; }
    std::cout << "[image] loaded " << width << "x" << height << " (24bppRGB)" << std::endl;
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
static const uint64_t VA_INPUT = 0x001000ULL;  // PTE[1..75]      input           ★ 0 금지
static const uint64_t VA_OUTPUT = 0x100000ULL;  // PTE[256..]     output          INPUT 너머
static const uint64_t VA_SCRATCH = 0x180000ULL;  // PTE[384..]    scratch         OUTPUT 너머
static const uint64_t VA_EXE1_PARAM_DATA = 0x200000ULL;  // PTE[512..2011] parameters  SCRATCH 너머
static const uint64_t VA_EXE0_PARAM_DATA = 0xA00000ULL;  // 192kb exe0 보조 
static const uint64_t VA_PARAM_BITSTREAM = 0x840000ULL;  // PTE[2112..2114] exe1 bitstream
static const uint64_t VA_INFER_BITSTREAM = 0x900000ULL;  // PTE[2304..2352] exe0 bitstream
static const uint64_t VA_EXE0_BITSTREAM_PHASE1 = 0x800000ULL;  // libedgetpu pattern


int main(int argc, char** argv)
{
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

    // -------------------------------------------------------------------------
    // input image 로드 → input slot 으로 직접 채움.
    // INPUT_SIZE = side*side*3 (24bpp RGB) 가정. square 모델만 지원.
    // -------------------------------------------------------------------------
    {
        UINT imgSide = static_cast<UINT>(std::sqrt(static_cast<double>(INPUT_SIZE) / 3.0));
        if (imgSide * imgSide * 3 != INPUT_SIZE) {
            std::cout << "[main] WARNING: input size " << INPUT_SIZE
                      << " is not square*3 — falling back to imgSide=" << imgSide << std::endl;
        }
        std::cout << "[main] loading image (target " << imgSide << "x" << imgSide << ")" << std::endl;
        if (!LoadJpegToRGB(L".\\assets\\karina.jpg", pInputBuf, imgSide, imgSide)) {
            std::cout << "[main] FAIL: image load" << std::endl;
            goto cleanup;
        }
    }

    DumpHex("after-PC slot0 head", (const void*)allocOut.InputUserVA, 0x0000, 0x100);
    DumpHex("after-PC slot0 0xe000", (const void*)allocOut.InputUserVA, 0xe000, 0x200);
    DumpHex("after-PC slot0 0xf000", (const void*)allocOut.InputUserVA, 0xf000, 0x100);

    // output slot 0 으로 — chip 이 outfeed 한 데이터인지 판별 가능하게
    memset(pOutputBuf, 0, OUTPUT_SIZE);
    if (pScratchBuf) memset(pScratchBuf, 0, SCRATCH_SIZE);

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
    }

    // -------------------------------------------------------------------------
    // outfeed inspect
    //   1) raw bytes (단순 chip outfeed 동작 확인용, add_int8 등에도 유용)
    //   2) per-layer dump (output 이 multi-layer 인 경우 각 layer 의 첫 float 들)
    //   3) SSD MobileNet detection 파싱 (layer 4개 [boxes/classes/scores/num] 가정)
    // -------------------------------------------------------------------------
    {
        PUCHAR output = (PUCHAR)pOutputBuf;

        // (1) raw byte summary
        size_t dumpN = std::min<size_t>(OUTPUT_SIZE, (size_t)16);
        size_t nonZero = 0;
        for (size_t i = 0; i < OUTPUT_SIZE; i++) if (output[i] != 0) nonZero++;
        std::cout << "[outfeed] OUTPUT_SIZE=" << OUTPUT_SIZE
                  << "  non-zero=" << nonZero << std::endl;
        std::cout << "[outfeed] first " << dumpN << " bytes:";
        std::cout << std::hex << std::setfill('0');
        for (size_t i = 0; i < dumpN; i++)
            std::cout << " " << std::setw(2) << (int)output[i];
        std::cout << std::dec << std::setfill(' ') << std::endl;

        // (2) per-layer dump — host buffer 에서 각 layer 는 PageAlignUp(prev) offset 부터 시작.
        // PatchVAs 의 Description_BASE_ADDRESS_OUTPUT_ACTIVATION 분기와 동일한 layout.
        std::cout << "\n[outfeed] per-layer dump (assume float32):" << std::endl;
        size_t off = 0;
        for (size_t li = 0; li < model.output_layers.size(); li++) {
            const auto& layer = model.output_layers[li];
            const float* fp = (const float*)(output + off);
            size_t nFloats = layer.size_bytes / sizeof(float);
            size_t showN = std::min<size_t>(nFloats, (size_t)8);
            std::cout << "  layer[" << li << "] '" << layer.name
                      << "' off=0x" << std::hex << off << std::dec
                      << " size=" << layer.size_bytes
                      << " floats[0.." << showN << "):";
            for (size_t i = 0; i < showN; i++)
                std::cout << " " << fp[i];
            std::cout << std::endl;
            off += apex_fb::PageAlignUp(layer.size_bytes);
        }

        // (3) SSD MobileNet face detection 파싱
        // postprocess 가 fused 된 모델은 output 4개:
        //   layer[0] = detection_boxes   [K,4]  (ymin, xmin, ymax, xmax) 정규화 [0,1]
        //   layer[1] = detection_classes [K]
        //   layer[2] = detection_scores  [K]
        //   layer[3] = num_detections    [1]
        if (model.output_layers.size() >= 4) {
            size_t off0 = 0;
            size_t off1 = off0 + apex_fb::PageAlignUp(model.output_layers[0].size_bytes);
            size_t off2 = off1 + apex_fb::PageAlignUp(model.output_layers[1].size_bytes);
            size_t off3 = off2 + apex_fb::PageAlignUp(model.output_layers[2].size_bytes);

            const float* boxes   = (const float*)(output + off0);
            const float* classes = (const float*)(output + off1);
            const float* scores  = (const float*)(output + off2);
            const float* numF    = (const float*)(output + off3);

            int numDet = (int)numF[0];
            int K      = (int)(model.output_layers[2].size_bytes / sizeof(float));
            UINT imgSide = static_cast<UINT>(std::sqrt(static_cast<double>(INPUT_SIZE) / 3.0));

            std::cout << "\n[SSD] num_detections=" << numDet << " (K=" << K << ")" << std::endl;
            const float kThresh = 0.3f;
            int kept = 0;
            for (int i = 0; i < numDet && i < K; i++) {
                float score = scores[i];
                if (score < kThresh) continue;
                float ymin = boxes[i*4 + 0];
                float xmin = boxes[i*4 + 1];
                float ymax = boxes[i*4 + 2];
                float xmax = boxes[i*4 + 3];
                int px1 = (int)(xmin * imgSide), py1 = (int)(ymin * imgSide);
                int px2 = (int)(xmax * imgSide), py2 = (int)(ymax * imgSide);
                std::cout << "  face[" << i << "] cls=" << (int)classes[i]
                          << " score=" << std::fixed << std::setprecision(3) << score
                          << " bbox(norm)=[" << ymin << "," << xmin << "," << ymax << "," << xmax << "]"
                          << " bbox(px)=[" << px1 << "," << py1 << "," << px2 << "," << py2 << "]"
                          << std::defaultfloat << std::endl;
                kept++;
            }
            std::cout << "[SSD] kept=" << kept << " (threshold=" << kThresh << ")" << std::endl;
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

