// npu_test_console.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <vector>
#include <fstream>
#include "../include/util.hpp"
#include "../include/Public.h"
#include <Windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "../lib/edgetpu.dll.if.lib")

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

int main()
{
    HANDLE handle = NULL;
    handle = FindPicoDriverDevice(GUID_DEVINTERFACE_npudriver);

    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        std::cout << "fail to get device handle: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "success to get device handle!" << std::endl;

    // Load actual model file
    std::vector<char> modelData = LoadModelFile(".\\models\\ssd_mobilenet_v2_face_quant_postprocess_edgetpu.tflite");

    if (modelData.empty()) {
        std::cout << "Failed to load model file" << std::endl;
        CloseHandle(handle);
        return 1;
    }

    size_t MODEL_SIZE = modelData.size();

    // Allocate page-aligned buffer for model
    void* pModelBuffer = AllocateAlignedMemory(MODEL_SIZE, 4096);

    if (pModelBuffer == nullptr) {
        std::cout << "Failed to allocate model buffer" << std::endl;
        CloseHandle(handle);
        return 1;
    }

    // Copy model data to aligned buffer
    memcpy(pModelBuffer, modelData.data(), MODEL_SIZE);
    std::cout << "Model buffer allocated at 0x" << std::hex << (UINT64)pModelBuffer
              << std::dec << " size: " << MODEL_SIZE << " bytes" << std::endl;

    // Prepare IOCTL input
    MAP_BUFFER_INPUT mapInput;
    mapInput.UserAddress = (UINT64)pModelBuffer;
    mapInput.Size = MODEL_SIZE;

    DWORD bytesReturned = 0;

    // Call IOCTL_MAP_BUFFER
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

    if (result) {
        std::cout << "IOCTL_MAP_BUFFER succeeded!" << std::endl;
    }
    else {
        std::cout << "IOCTL_MAP_BUFFER failed: " << GetLastError() << std::endl;
    }

    // Test IOCTL_UNMAP_BUFFER
    UNMAP_BUFFER_INPUT unmapInput;
    unmapInput.DeviceAddress = 0;  // Device virtual address
    unmapInput.Size = MODEL_SIZE;

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
    FreeAlignedMemory(pModelBuffer);
    CloseHandle(handle);

    std::cout << "Test completed!" << std::endl;

    MessageBox(NULL, L"wait", L"wait", NULL);

    return 0;
}
