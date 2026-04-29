// npu_test_console.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <vector>
#include <fstream>
#include <cmath>

// Must define NOMINMAX before including Windows.h to avoid macro conflicts with std::max/min
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>

#include "../include/util.hpp"
#include "../include/Public.h"
#include "../include/apex_model_fb.hpp"  // Official FlatBuffers parser with layer metadata

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "../lib/edgetpu.dll.if.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Load model file from disk
std::vector<char> LoadModelFile(const char* filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open model file: " << filename << std::endl;
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "Failed to read model file" << std::endl;
        return {};
    }

    std::cout << "Model loaded: " << size << " bytes" << std::endl;
    return buffer;
}

// Helper function to allocate aligned memory
void* AllocateAlignedMemory(size_t size, size_t alignment)
{
    void* ptr = _aligned_malloc(size, alignment);
    if (ptr == nullptr) {
        std::cerr << "Failed to allocate aligned memory" << std::endl;
        return nullptr;
    }
    return ptr;
}

void FreeAlignedMemory(void* ptr)
{
    if (ptr) {
        _aligned_free(ptr);
    }
}

// Load JPEG image, resize to 320x320, convert to 24bpp RGB
bool LoadJpegToRGB(const wchar_t* path, void* outBuf, UINT width, UINT height)
{
    HRESULT hr = S_OK;
    IWICImagingFactory* pFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICBitmapScaler* pScaler = nullptr;
    IWICFormatConverter* pConverter = nullptr;

    // Create WIC factory
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void**)&pFactory);
    if (FAILED(hr)) {
        std::cerr << "CoCreateInstance failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Create decoder from filename
    hr = pFactory->CreateDecoderFromFilename(path, nullptr,
                                             GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                             &pDecoder);
    if (FAILED(hr)) {
        std::cerr << "CreateDecoderFromFilename failed: 0x" << std::hex << hr << std::endl;
        pFactory->Release();
        return false;
    }

    // Get first frame
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) {
        std::cerr << "GetFrame failed: 0x" << std::hex << hr << std::endl;
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    // Create scaler to resize to target dimensions
    hr = pFactory->CreateBitmapScaler(&pScaler);
    if (FAILED(hr)) {
        std::cerr << "CreateBitmapScaler failed: 0x" << std::hex << hr << std::endl;
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    hr = pScaler->Initialize(pFrame, width, height, WICBitmapInterpolationModeCubic);
    if (FAILED(hr)) {
        std::cerr << "Scaler Initialize failed: 0x" << std::hex << hr << std::endl;
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    // Create format converter to RGB
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) {
        std::cerr << "CreateFormatConverter failed: 0x" << std::hex << hr << std::endl;
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat24bppRGB,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        std::cerr << "Converter Initialize failed: 0x" << std::hex << hr << std::endl;
        pConverter->Release();
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    // Copy pixels to output buffer
    UINT stride = width * 3;  // 24bpp = 3 bytes/pixel
    UINT bufferSize = stride * height;
    hr = pConverter->CopyPixels(nullptr, stride, bufferSize, (BYTE*)outBuf);
    if (FAILED(hr)) {
        std::cerr << "CopyPixels failed: 0x" << std::hex << hr << std::endl;
        pConverter->Release();
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return false;
    }

    // Cleanup
    pConverter->Release();
    pScaler->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();

    std::cout << "Image loaded: " << width << "x" << height << " (24bppRGB)" << std::endl;
    return true;
}

int main()
{
    HANDLE handle = NULL;
    handle = FindPicoDriverDevice(GUID_DEVINTERFACE_npudriver);

    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        std::cout << "fail to get device handle: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "success to get device handle!" << std::endl;

    // Initialize COM for WIC
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 1. Load model using apex_model_fb.hpp
    std::cout << "Loading model..." << std::endl;
    apex_fb::ApexModelFb model = apex_fb::LoadModel(".\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");

    if (model.bitstream.empty()) {
        std::cout << "Failed to load or parse model file" << std::endl;
        CloseHandle(handle);
        return 1;
    }

    if (model.input_layers.empty() || model.output_layers.empty()) {
        std::cout << "Model has no input or output layers" << std::endl;
        CloseHandle(handle);
        return 1;
    }

    std::cout << "Model loaded: bitstream size = " << model.bitstream.size() << " bytes" << std::endl;
    std::cout << "Field patches to apply: " << model.patches.size() << std::endl;
    std::cout << "Input layer: " << model.input_layers[0].name << " (size: " << model.input_layers[0].size_bytes << " bytes)" << std::endl;
    std::cout << "Output layer: " << model.output_layers[0].name << " (size: " << model.output_layers[0].size_bytes << " bytes)" << std::endl;
    

    DWORD bytesReturned = 0;

    // PARAM device VA — INFER bitstream 도 같은 값으로 패치되어야 한다 (libedgetpu
    // MapParameters 패턴: PARAMETER_CACHING / main 양쪽 executable 이 같은 device VA 의
    // parameter buffer 를 참조함). Phase 1 가 없는 STAND_ALONE 모델에서는 0.
    uint64_t param_va = 0;
    void* pParametersBuf = nullptr;  // INFER 동안 살아있어야 함 (kernel MDL pinned)
    bool useInferWithParam = false;  // Phase 1 가 있으면 IOCTL_INFER_WITH_PARAM 사용

    // Phase 1: Parameter caching — load weights into on-chip cache via PARAMETER_CACHING executable
    if (!model.param_bitstream.empty() && !model.parameters.empty()) {
        useInferWithParam = true;
        std::cout << "\n--- Phase 1: Parameter Caching ---" << std::endl;

        // param_va: device VA for parameters data, immediately after exe1 bitstream
        param_va = apex_fb::PageAlignUp(model.param_bitstream.size());
        apex_fb::PatchParamBitstreamVAs(model, param_va);
        std::cout << "exe1 bitstream patched:" << std::endl;
        apex_fb::DumpParamPatchedVAs(model);

        void* pParamBitstreamBuf = AllocateAlignedMemory(model.param_bitstream.size(), 4096);
        pParametersBuf           = AllocateAlignedMemory(model.parameters.size(), 4096);

        if (pParamBitstreamBuf == nullptr || pParametersBuf == nullptr) {
            std::cout << "Failed to allocate Phase 1 buffers" << std::endl;
            if (pParamBitstreamBuf) FreeAlignedMemory(pParamBitstreamBuf);
            if (pParametersBuf)     FreeAlignedMemory(pParametersBuf);
            CloseHandle(handle);
            CoUninitialize();
            return 1;
        }

        memcpy(pParamBitstreamBuf, model.param_bitstream.data(), model.param_bitstream.size());
        memcpy(pParametersBuf, model.parameters.data(), model.parameters.size());
        std::cout << "Phase 1 buffers: bitstream=" << model.param_bitstream.size()
                  << "B  parameters=" << model.parameters.size() << "B" << std::endl;

        // Map exe1 bitstream at device VA 0x0 (PTE[0..N1])
        MAP_BUFFER_INPUT mapP1 = {};
        mapP1.UserAddress = (UINT64)pParamBitstreamBuf;
        mapP1.Size = model.param_bitstream.size();
        mapP1.DeviceAddress = 0;
        bool p1Ok = DeviceIoControl(handle, IOCTL_MAP_BUFFER, &mapP1, sizeof(MAP_BUFFER_INPUT),
                                    nullptr, 0, &bytesReturned, nullptr) != 0;
        if (!p1Ok) {
            std::cout << "Phase 1 IOCTL_MAP_BUFFER failed: " << GetLastError() << std::endl;
            FreeAlignedMemory(pParametersBuf);
            FreeAlignedMemory(pParamBitstreamBuf);
            CloseHandle(handle);
            CoUninitialize();
            return 1;
        }
        std::cout << "Phase 1: exe1 bitstream mapped at device VA 0x0" << std::endl;

        // ===== FULL PARAMETER_CACHING MODE =====
        // PARAMETER_CACHING bitstream 을 IQ 에 제출 → 하드웨어가 weights 를 on-chip
        // SRAM 으로 DMA 해 캐시. 이게 INFER 의 전제 조건. 끝에 halt 명령으로 INFEED/
        // PARAM 이 kHalted=0x4 로 떨어지지만, on-chip 에는 weights 가 올라가있음.
        // INFER 진입 직전 엔진을 깨우는 방법을 찾는 게 다음 과제.
        IOCTL_PARAM_CACHE_INFO pci = {};
        pci.ParamAddr     = (UINT64)pParametersBuf;
        pci.ParamSize     = model.parameters.size();
        pci.ParamDeviceVA = param_va;
        pci.BitstreamSize = model.param_bitstream.size();  // ← bitstream 정상 제출
        p1Ok = DeviceIoControl(handle, IOCTL_PARAM_CACHE, &pci, sizeof(IOCTL_PARAM_CACHE_INFO),
                               nullptr, 0, &bytesReturned, nullptr) != 0;
        std::cout << (p1Ok ? "IOCTL_PARAM_CACHE succeeded! Weights cached on-chip."
                           : "IOCTL_PARAM_CACHE FAILED!") << std::endl;
        if (!p1Ok) std::cout << "  error: " << GetLastError() << std::endl;

        // NOTE: Driver retains the PARAM bitstream (PTE[0..N1]) and its MDL after
        // IOCTL_PARAM_CACHE — IOCTL_INFER_WITH_PARAM re-enqueues that descriptor
        // back-to-back with INFER each call (libedgetpu pattern). DO NOT call
        // IOCTL_UNMAP_BUFFER on the PARAM bitstream here, and DO NOT free the
        // user buffer (driver MDL pins those pages until file handle close).

        if (!p1Ok) {
            FreeAlignedMemory(pParametersBuf);
            CloseHandle(handle);
            CoUninitialize();
            return 1;
        }
        std::cout << "--- Phase 1 Complete ---\n" << std::endl;
    } else {
        std::cout << "No parameter caching needed (STAND_ALONE model)" << std::endl;
    }

    // 2. Calculate device VAs — params 영역 (PTE[3..1502], 0x3000~0x5DF000) 과 충돌 금지.
    //    INFER bitstream 을 params 뒤로 이동: [params@0x3000] ... [bitstream@bitstream_va] [input] [output] [scratch]
    const size_t INPUT_SIZE   = model.input_layers[0].size_bytes;
    const size_t OUTPUT_SIZE  = model.total_output_size_bytes;  // all output layers, page-aligned
    const size_t SCRATCH_SIZE = model.scratch_size_bytes;
    // 0x600000 = 6MB, params (0x3000~0x5DEB00, ~6MB) 끝나고 충분한 여유
    uint64_t bitstream_va = 0x600000ULL;
    uint64_t input_va   = bitstream_va + apex_fb::PageAlignUp(model.bitstream.size());
    uint64_t output_va  = input_va     + apex_fb::PageAlignUp(INPUT_SIZE);
    uint64_t scratch_va = (SCRATCH_SIZE > 0)
                          ? output_va + apex_fb::PageAlignUp(OUTPUT_SIZE)
                          : 0;
    std::cout << "Bitstream device VA: 0x" << std::hex << bitstream_va << std::dec << std::endl;

    std::cout << "Input device VA:   0x" << std::hex << input_va   << std::dec << std::endl;
    std::cout << "Output device VA:  0x" << std::hex << output_va  << std::dec << std::endl;
    std::cout << "Scratch device VA: 0x" << std::hex << scratch_va << std::dec
              << " (size=" << SCRATCH_SIZE << ")" << std::endl;

    // 3. Patch bitstream with all device VAs (must happen before MAP)
    std::cout << "--- VA map (before patch) ---" << std::endl;
    apex_fb::DumpPatchedVAs(model);

    // INFER bitstream 도 PARAMETER_CACHING 과 같은 device VA 로 PARAM 패치 (libedgetpu
    // single_tpu_request.cc:77 + instruction_buffers.cc:79~ 패턴: 양쪽 executable 이
    // 같은 parameter_device_buffer.device_address() 를 link 함).
    apex_fb::PatchVAs(model, input_va, output_va, param_va, scratch_va);

    std::cout << "--- VA map (after patch) ---" << std::endl;
    apex_fb::DumpPatchedVAs(model);
    std::cout << "Bitstream patched successfully" << std::endl;

    // 4. Allocate page-aligned buffer and copy patched bitstream
    void* pModelBuffer = AllocateAlignedMemory(model.bitstream.size(), 4096);
    if (pModelBuffer == nullptr) {
        std::cout << "Failed to allocate model buffer" << std::endl;
        CloseHandle(handle);
        return 1;
    }

    memcpy(pModelBuffer, model.bitstream.data(), model.bitstream.size());
    std::cout << "Model buffer allocated at 0x" << std::hex << (UINT64)pModelBuffer
              << std::dec << " size: " << model.bitstream.size() << " bytes" << std::endl;

    // 5. Map patched bitstream at bitstream_va (params 뒤쪽 — PTE 충돌 방지)
    MAP_BUFFER_INPUT mapInput = {};
    mapInput.UserAddress = (UINT64)pModelBuffer;
    mapInput.Size = model.bitstream.size();
    mapInput.DeviceAddress = bitstream_va;

    std::cout << "Mapping model buffer..." << std::endl;
    BOOL result = DeviceIoControl(
        handle,
        IOCTL_MAP_BUFFER,
        &mapInput,
        sizeof(MAP_BUFFER_INPUT),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (!result) {
        std::cout << "IOCTL_MAP_BUFFER failed: " << GetLastError() << std::endl;
        FreeAlignedMemory(pModelBuffer);
        CloseHandle(handle);
        return 1;
    }
    std::cout << "IOCTL_MAP_BUFFER succeeded!" << std::endl;

    // 6. Allocate input image, output buffer, and scratch buffer
    void* pImageBuf   = AllocateAlignedMemory(INPUT_SIZE, 4096);
    void* pOutputBuf  = AllocateAlignedMemory(OUTPUT_SIZE, 4096);
    void* pScratchBuf = (SCRATCH_SIZE > 0) ? AllocateAlignedMemory(SCRATCH_SIZE, 4096) : nullptr;

    if (pImageBuf == nullptr || pOutputBuf == nullptr ||
        (SCRATCH_SIZE > 0 && pScratchBuf == nullptr)) {
        std::cout << "Failed to allocate image/output/scratch buffers" << std::endl;
        if (pImageBuf)   FreeAlignedMemory(pImageBuf);
        if (pOutputBuf)  FreeAlignedMemory(pOutputBuf);
        if (pScratchBuf) FreeAlignedMemory(pScratchBuf);
        FreeAlignedMemory(pModelBuffer);
        CloseHandle(handle);
        return 1;
    }

    if (pScratchBuf) {
        memset(pScratchBuf, 0, SCRATCH_SIZE);
        std::cout << "Scratch buffer allocated at 0x" << std::hex << (UINT64)pScratchBuf
                  << std::dec << " size: " << SCRATCH_SIZE << " bytes" << std::endl;
    }

    // Load real image — dimension은 모델에서 동적으로 도출.
    // 24bpp RGB 가정 (3 channels). square 이미지 가정 (224x224, 300x300, 320x320 등).
    UINT imgSide = static_cast<UINT>(std::sqrt(static_cast<double>(INPUT_SIZE) / 3.0));
    if (imgSide * imgSide * 3 != INPUT_SIZE) {
        std::cout << "WARNING: input size " << INPUT_SIZE
                  << " is not square*3 — falling back to imgSide=" << imgSide << std::endl;
    }
    std::cout << "Loading image from assets/aespa.jpg... (target " << imgSide << "x" << imgSide << ")" << std::endl;
    if (!LoadJpegToRGB(L".\\assets\\karina.jpg", pImageBuf, imgSide, imgSide)) {
        std::cout << "Failed to load image" << std::endl;
        if (pScratchBuf) FreeAlignedMemory(pScratchBuf);
        FreeAlignedMemory(pOutputBuf);
        FreeAlignedMemory(pImageBuf);
        FreeAlignedMemory(pModelBuffer);
        CloseHandle(handle);
        CoUninitialize();
        return 1;
    }

    // Clear output buffer
    memset(pOutputBuf, 0, OUTPUT_SIZE);

    std::cout << "Image buffer allocated at 0x" << std::hex << (UINT64)pImageBuf << std::dec << std::endl;
    std::cout << "Output buffer allocated at 0x" << std::hex << (UINT64)pOutputBuf << std::dec << std::endl;

    // 7. Call IOCTL_INFER (or IOCTL_INFER_WITH_PARAM if Phase 1 ran).
    //    INFER_WITH_PARAM tells the driver to enqueue the cached PARAM bitstream
    //    descriptor immediately before INFER in the same IQ batch — libedgetpu
    //    pattern that prevents engine auto-halt between PARAM_CACHE and INFER.
    DWORD inferIoctl = useInferWithParam ? IOCTL_INFER_WITH_PARAM : IOCTL_INFER;
    std::cout << "Calling " << (useInferWithParam ? "IOCTL_INFER_WITH_PARAM" : "IOCTL_INFER")
              << "..." << std::endl;
    IOCTL_INFER_INFO inferInput = {};
    inferInput.InputImageAddr    = (UINT64)pImageBuf;
    inferInput.InputImageSize    = INPUT_SIZE;
    inferInput.OutputBufferAddr  = (UINT64)pOutputBuf;
    inferInput.OutputBufferSize  = OUTPUT_SIZE;
    inferInput.InputDeviceVA     = input_va;
    inferInput.OutputDeviceVA    = output_va;
    inferInput.BitstreamDeviceVA = bitstream_va;
    inferInput.BitstreamSize     = model.bitstream.size();
    inferInput.ScratchAddr       = (UINT64)pScratchBuf;
    inferInput.ScratchSize       = SCRATCH_SIZE;
    inferInput.ScratchDeviceVA   = scratch_va;

    result = DeviceIoControl(
        handle,
        inferIoctl,
        &inferInput,
        sizeof(IOCTL_INFER_INFO),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (result) {
        std::cout << "INFER succeeded!" << std::endl;
    }
    else {
        std::cout << "INFER failed: " << GetLastError() << std::endl;
    }

    // Output buffer dump — check if TPU wrote anything regardless of timeout
    {
        const uint8_t* out = reinterpret_cast<const uint8_t*>(pOutputBuf);
        const size_t dumpBytes = std::min(OUTPUT_SIZE, (size_t)64);

        // Count non-zero bytes in the full output
        size_t nonZero = 0;
        for (size_t i = 0; i < OUTPUT_SIZE; i++)
            if (out[i] != 0) nonZero++;

        std::cout << "\n--- Output buffer (" << OUTPUT_SIZE << " bytes, "
                  << nonZero << " non-zero) ---" << std::endl;

        // Hex dump of first 64 bytes
        std::cout << std::hex << std::setfill('0');
        for (size_t i = 0; i < dumpBytes; i++) {
            if (i % 16 == 0) std::cout << "  [" << std::setw(4) << i << "] ";
            std::cout << std::setw(2) << (int)out[i] << " ";
            if (i % 16 == 15) std::cout << "\n";
        }
        std::cout << std::dec << std::endl;
    }

    // 8. Unmap INFER bitstream (params 는 driver 가 FileCleanup 에서 해제)
    std::cout << "Unmapping model buffer..." << std::endl;
    UNMAP_BUFFER_INPUT unmapInput;
    unmapInput.DeviceAddress = bitstream_va;
    unmapInput.Size = model.bitstream.size();

    result = DeviceIoControl(
        handle,
        IOCTL_UNMAP_BUFFER,
        &unmapInput,
        sizeof(UNMAP_BUFFER_INPUT),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (result) {
        std::cout << "IOCTL_UNMAP_BUFFER succeeded!" << std::endl;
    }
    else {
        std::cout << "IOCTL_UNMAP_BUFFER failed: " << GetLastError() << std::endl;
    }

    // Cleanup — CloseHandle 이 driver 의 FileCleanup 을 트리거해 CachedParamMdl 을
    // 안전하게 unlock 한 뒤 파라미터 user buffer 를 free.
    FreeAlignedMemory(pImageBuf);
    FreeAlignedMemory(pOutputBuf);
    if (pScratchBuf) FreeAlignedMemory(pScratchBuf);
    FreeAlignedMemory(pModelBuffer);
    CloseHandle(handle);
    if (pParametersBuf) FreeAlignedMemory(pParametersBuf);
    CoUninitialize();

    std::cout << "Test completed!" << std::endl;

    MessageBox(NULL, L"wait", L"wait", NULL);

    return 0;
}
