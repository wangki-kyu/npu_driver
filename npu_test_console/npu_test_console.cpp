// npu_test_console.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <vector>
#include <fstream>

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

    // 2. Calculate device VAs (input after bitstream, output after input)
    const size_t INPUT_SIZE  = model.input_layers[0].size_bytes;
    const size_t OUTPUT_SIZE = model.output_layers[0].size_bytes;
    uint64_t input_va  = apex_fb::PageAlignUp(model.bitstream.size());
    uint64_t output_va = input_va + apex_fb::PageAlignUp(INPUT_SIZE);

    std::cout << "Input device VA:  0x" << std::hex << input_va << std::dec << std::endl;
    std::cout << "Output device VA: 0x" << std::hex << output_va << std::dec << std::endl;

    // 3. Patch bitstream with device VAs
    std::cout << "Patching bitstream..." << std::endl;
    apex_fb::PatchVAs(model, input_va, output_va);
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

    // 5. Map patched bitstream
    MAP_BUFFER_INPUT mapInput;
    mapInput.UserAddress = (UINT64)pModelBuffer;
    mapInput.Size = model.bitstream.size();

    DWORD bytesReturned = 0;

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

    // 6. Allocate input image and output buffer
    void* pImageBuf  = AllocateAlignedMemory(INPUT_SIZE, 4096);
    void* pOutputBuf = AllocateAlignedMemory(OUTPUT_SIZE, 4096);

    if (pImageBuf == nullptr || pOutputBuf == nullptr) {
        std::cout << "Failed to allocate image/output buffers" << std::endl;
        if (pImageBuf) FreeAlignedMemory(pImageBuf);
        if (pOutputBuf) FreeAlignedMemory(pOutputBuf);
        FreeAlignedMemory(pModelBuffer);
        CloseHandle(handle);
        return 1;
    }

    // Load real image
    std::cout << "Loading image from assets/aespa.jpg..." << std::endl;
    if (!LoadJpegToRGB(L".\\assets\\aespa.jpg", pImageBuf, 320, 320)) {
        std::cout << "Failed to load image" << std::endl;
        FreeAlignedMemory(pImageBuf);
        FreeAlignedMemory(pOutputBuf);
        FreeAlignedMemory(pModelBuffer);
        CloseHandle(handle);
        CoUninitialize();
        return 1;
    }

    // Clear output buffer
    memset(pOutputBuf, 0, OUTPUT_SIZE);

    std::cout << "Image buffer allocated at 0x" << std::hex << (UINT64)pImageBuf << std::dec << std::endl;
    std::cout << "Output buffer allocated at 0x" << std::hex << (UINT64)pOutputBuf << std::dec << std::endl;

    // 7. Call IOCTL_INFER
    std::cout << "Calling IOCTL_INFER..." << std::endl;
    IOCTL_INFER_INFO inferInput = {};
    inferInput.InputImageAddr    = (UINT64)pImageBuf;
    inferInput.InputImageSize    = INPUT_SIZE;
    inferInput.OutputBufferAddr  = (UINT64)pOutputBuf;
    inferInput.OutputBufferSize  = OUTPUT_SIZE;
    inferInput.InputDeviceVA     = input_va;
    inferInput.OutputDeviceVA    = output_va;
    inferInput.BitstreamDeviceVA = 0;  // Bitstream is at device VA 0x0
    inferInput.BitstreamSize     = model.bitstream.size();

    result = DeviceIoControl(
        handle,
        IOCTL_INFER,
        &inferInput,
        sizeof(IOCTL_INFER_INFO),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (result) {
        std::cout << "IOCTL_INFER succeeded!" << std::endl;
    }
    else {
        std::cout << "IOCTL_INFER failed: " << GetLastError() << std::endl;
    }

    // 8. Unmap buffer
    std::cout << "Unmapping model buffer..." << std::endl;
    UNMAP_BUFFER_INPUT unmapInput;
    unmapInput.DeviceAddress = 0;
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

    // Cleanup
    FreeAlignedMemory(pImageBuf);
    FreeAlignedMemory(pOutputBuf);
    FreeAlignedMemory(pModelBuffer);
    CloseHandle(handle);
    CoUninitialize();

    std::cout << "Test completed!" << std::endl;

    MessageBox(NULL, L"wait", L"wait", NULL);

    return 0;
}
