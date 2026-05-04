// add_model_test_console.cpp
//
// Minimal test console for Edge TPU outfeed verification.
//   - Model:   model/add_int8_edgetpu.tflite  (y = x + 1, 16-byte int8)
//   - infeed:  golden.input  = f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
//   - outfeed: golden.output = f8 f9 fa fb fc fd fe ff 00 01 02 03 04 05 06 07
//
// Goal: verify that the KMDF driver actually delivers chip outfeed bytes into
// the host output buffer. Stripped of image loading, SSD post-processing, etc.
// Completely separate from npu_test_console -- no side effects.

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

#define NOMINMAX
#include <Windows.h>

#include "../include/util.hpp"
#include "../include/Public.h"
#include "../include/apex_model_fb.hpp"

#pragma comment(lib, "user32.lib")

// -----------------------------------------------------------------------------
// Constants: golden vector (model/golden.json -> add_int8 entry)
// -----------------------------------------------------------------------------
static const uint8_t kGoldenInput[16] = {
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
};
static const uint8_t kGoldenOutput[16] = {
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
};

// Extended VA mode (matches libedgetpu working trace layout pattern).
static const uint64_t EXT_VA_BIT = 0x8000000000000000ULL;
static const uint64_t VA_PARAM_DATA      = EXT_VA_BIT | 0x000000ULL;  // exe1 parameters
static const uint64_t VA_PARAM_BITSTREAM = EXT_VA_BIT | 0x840000ULL;  // exe1 bitstream
static const uint64_t VA_INFER_BITSTREAM = EXT_VA_BIT | 0x900000ULL;  // exe0 bitstream (INFER)
static const uint64_t VA_INPUT           = EXT_VA_BIT | 0x880000ULL;
static const uint64_t VA_OUTPUT          = EXT_VA_BIT | 0x844000ULL;
static const uint64_t VA_SCRATCH         = EXT_VA_BIT | 0x848000ULL;
static const uint64_t VA_EXE0_BITSTREAM_PHASE1 = EXT_VA_BIT | 0x800000ULL; // libedgetpu pattern

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------
static void* AlignedAlloc4K(size_t size) {
    return _aligned_malloc(size, 4096);
}
static void AlignedFree(void* p) { if (p) _aligned_free(p); }

static void HexDump(const char* label, const uint8_t* data, size_t n) {
    std::cout << "  " << label << ":";
    std::cout << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; i++) {
        std::cout << " " << std::setw(2) << (int)data[i];
    }
    std::cout << std::dec << std::endl;
}

// add_int8 quantization parameters (from golden.json)
static const double kInputScale  = 0.007837736047804356;
static const int    kInputZP     = 0;
static const double kOutputScale = 0.00783922616392374;
static const int    kOutputZP    = -128;

static void DumpRealValues(const uint8_t* in, const uint8_t* out, size_t n) {
    std::cout << "  per-element real value (add_int8: y = x + 1):" << std::endl;
    std::cout << "    idx  in_byte  in_int8  in_real     out_byte  out_int8  out_real    expected_out_real (in_real+1.0)" << std::endl;
    for (size_t i = 0; i < n; i++) {
        int8_t in_s8  = (int8_t)in[i];
        int8_t out_s8 = (int8_t)out[i];
        double in_real  = (in_s8  - kInputZP)  * kInputScale;
        double out_real = (out_s8 - kOutputZP) * kOutputScale;
        double expected = in_real + 1.0;
        std::cout << "    " << std::setw(3) << i
                  << "    0x" << std::hex << std::setw(2) << std::setfill('0') << (int)in[i] << std::setfill(' ') << std::dec
                  << "      " << std::setw(4) << (int)in_s8
                  << "    " << std::fixed << std::setprecision(4) << std::setw(8) << in_real
                  << "    0x" << std::hex << std::setw(2) << std::setfill('0') << (int)out[i] << std::setfill(' ') << std::dec
                  << "       " << std::setw(4) << (int)out_s8
                  << "    " << std::fixed << std::setprecision(4) << std::setw(8) << out_real
                  << "    " << std::fixed << std::setprecision(4) << std::setw(8) << expected
                  << std::endl;
    }
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char** argv)
{
    // Model path: prefer argv[1], otherwise probe a few common candidates.
    std::vector<std::string> candidates;
    if (argc >= 2) candidates.push_back(argv[1]);
    candidates.push_back(".\\models\\add_int8_edgetpu.tflite");
    candidates.push_back("..\\models\\add_int8_edgetpu.tflite");
    candidates.push_back("..\\..\\models\\add_int8_edgetpu.tflite");
    candidates.push_back("..\\..\\..\\models\\add_int8_edgetpu.tflite");
    candidates.push_back("E:\\work\\project\\npu_driver\\model\\add_int8_edgetpu.tflite");

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
        std::cout << "[main] FAIL: could not load add_int8_edgetpu.tflite from any candidate path" << std::endl;
        return 1;
    }
    std::cout << "[main] model loaded from: " << used_path << std::endl;

    if (model.input_layers.empty() || model.output_layers.empty()) {
        std::cout << "[main] FAIL: model has no input or output layers" << std::endl;
        return 1;
    }
    const size_t INPUT_SIZE   = model.input_layers[0].size_bytes;
    const size_t OUTPUT_SIZE  = model.total_output_size_bytes;
    const size_t SCRATCH_SIZE = model.scratch_size_bytes;
    std::cout << "[main] input_size=" << INPUT_SIZE
              << "  output_size=" << OUTPUT_SIZE
              << "  scratch_size=" << SCRATCH_SIZE << std::endl;

    if (INPUT_SIZE != 16 || OUTPUT_SIZE < 16) {
        std::cout << "[main] WARNING: expected 16-byte in/out for add_int8, got "
                  << INPUT_SIZE << "/" << OUTPUT_SIZE
                  << " -- proceeding but golden compare may fail if shape mismatched" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Device handle
    // -------------------------------------------------------------------------
    HANDLE handle = FindPicoDriverDevice(GUID_DEVINTERFACE_npudriver);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        std::cout << "[main] FAIL: device handle: " << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "[main] device handle OK" << std::endl;

    DWORD bytesReturned = 0;
    int   exitCode = 1;

    // User buffer pointers -- single cleanup path frees them all.
    void* pParamData      = nullptr; // exe1.parameters() data
    void* pParamBitstream = nullptr; // exe1 bitstream
    void* pExe0Phase1Bs   = nullptr; // exe0 bitstream copy mapped during Phase1
    void* pInferBitstream = nullptr; // exe0 bitstream (patched) for INFER mapping
    void* pInputBuf       = nullptr;
    void* pOutputBuf      = nullptr;
    void* pScratchBuf     = nullptr;

    bool useInferWithParam = false;
    uint64_t param_va = 0;

    // -------------------------------------------------------------------------
    // Phase 1: parameter caching (only if model has it)
    // -------------------------------------------------------------------------
    if (!model.param_bitstream.empty() && !model.parameters.empty()) {
        useInferWithParam = true;
        std::cout << "\n--- Phase 1: parameter caching ---" << std::endl;

        param_va = VA_PARAM_DATA;
        apex_fb::PatchParamBitstreamVAs(model, param_va);
        apex_fb::DumpParamPatchedVAs(model);

        pParamBitstream = AlignedAlloc4K(model.param_bitstream.size());
        pParamData      = AlignedAlloc4K(model.parameters.size());
        if (!pParamBitstream || !pParamData) {
            std::cout << "[main] FAIL: alloc Phase1 buffers" << std::endl;
            goto cleanup;
        }
        memcpy(pParamBitstream, model.param_bitstream.data(), model.param_bitstream.size());
        memcpy(pParamData,      model.parameters.data(),       model.parameters.size());

        // map exe1 bitstream
        MAP_BUFFER_INPUT mapPb = {};
        mapPb.UserAddress   = (UINT64)pParamBitstream;
        mapPb.Size          = model.param_bitstream.size();
        mapPb.DeviceAddress = VA_PARAM_BITSTREAM;
        if (!DeviceIoControl(handle, IOCTL_MAP_BUFFER, &mapPb, sizeof(mapPb),
                             nullptr, 0, &bytesReturned, nullptr)) {
            std::cout << "[main] FAIL: IOCTL_MAP_BUFFER (param bitstream): " << GetLastError() << std::endl;
            goto cleanup;
        }
        std::cout << "[main] mapped param bitstream @ 0x"
                  << std::hex << VA_PARAM_BITSTREAM << std::dec << std::endl;

        // PARAM_CACHE: DMA weights into on-chip SRAM
        IOCTL_PARAM_CACHE_INFO pci = {};
        pci.ParamAddr     = (UINT64)pParamData;
        pci.ParamSize     = model.parameters.size();
        pci.ParamDeviceVA = param_va;
        pci.BitstreamSize = model.param_bitstream.size();
        if (!DeviceIoControl(handle, IOCTL_PARAM_CACHE, &pci, sizeof(pci),
                             nullptr, 0, &bytesReturned, nullptr)) {
            std::cout << "[main] FAIL: IOCTL_PARAM_CACHE: " << GetLastError() << std::endl;
            goto cleanup;
        }
        std::cout << "[main] IOCTL_PARAM_CACHE OK (weights cached on-chip)" << std::endl;

        // libedgetpu pattern: also pre-map exe0 bitstream right after PARAM_CACHE
        // so PARAMETER_POP can dereference it during INFER.
        pExe0Phase1Bs = AlignedAlloc4K(model.bitstream.size());
        if (!pExe0Phase1Bs) {
            std::cout << "[main] FAIL: alloc exe0 phase1 bitstream" << std::endl;
            goto cleanup;
        }
        memcpy(pExe0Phase1Bs, model.bitstream.data(), model.bitstream.size());
        MAP_BUFFER_INPUT mapExe0p1 = {};
        mapExe0p1.UserAddress   = (UINT64)pExe0Phase1Bs;
        mapExe0p1.Size          = model.bitstream.size();
        mapExe0p1.DeviceAddress = VA_EXE0_BITSTREAM_PHASE1;
        if (!DeviceIoControl(handle, IOCTL_MAP_BUFFER, &mapExe0p1, sizeof(mapExe0p1),
                             nullptr, 0, &bytesReturned, nullptr)) {
            std::cout << "[main] WARNING: phase1 exe0 bitstream MAP failed (continuing): "
                      << GetLastError() << std::endl;
            AlignedFree(pExe0Phase1Bs);
            pExe0Phase1Bs = nullptr;
        } else {
            std::cout << "[main] mapped phase1 exe0 bitstream @ 0x"
                      << std::hex << VA_EXE0_BITSTREAM_PHASE1 << std::dec << std::endl;
        }
        std::cout << "--- Phase 1 done ---\n" << std::endl;
    } else {
        std::cout << "[main] no parameter caching (STAND_ALONE model)" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Patch INFER bitstream and map it
    // -------------------------------------------------------------------------
    {
        const uint64_t scratch_va_eff = (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0;
        std::cout << "[main] device VAs:"
                  << " bitstream=0x" << std::hex << VA_INFER_BITSTREAM
                  << " input=0x"     << VA_INPUT
                  << " output=0x"    << VA_OUTPUT
                  << " scratch=0x"   << scratch_va_eff
                  << " param=0x"     << param_va
                  << std::dec << std::endl;

        std::cout << "[main] VA map BEFORE patch:" << std::endl;
        apex_fb::DumpPatchedVAs(model);

        apex_fb::PatchVAs(model, VA_INPUT, VA_OUTPUT, param_va, scratch_va_eff);

        std::cout << "[main] VA map AFTER patch:" << std::endl;
        apex_fb::DumpPatchedVAs(model);
        apex_fb::DumpPatchRawValues(model, "POST-PATCH");

        pInferBitstream = AlignedAlloc4K(model.bitstream.size());
        if (!pInferBitstream) {
            std::cout << "[main] FAIL: alloc INFER bitstream buffer" << std::endl;
            goto cleanup;
        }
        memcpy(pInferBitstream, model.bitstream.data(), model.bitstream.size());

        MAP_BUFFER_INPUT mapBs = {};
        mapBs.UserAddress   = (UINT64)pInferBitstream;
        mapBs.Size          = model.bitstream.size();
        mapBs.DeviceAddress = VA_INFER_BITSTREAM;
        if (!DeviceIoControl(handle, IOCTL_MAP_BUFFER, &mapBs, sizeof(mapBs),
                             nullptr, 0, &bytesReturned, nullptr)) {
            std::cout << "[main] FAIL: IOCTL_MAP_BUFFER (infer bitstream): " << GetLastError() << std::endl;
            goto cleanup;
        }
        std::cout << "[main] mapped INFER bitstream @ 0x"
                  << std::hex << VA_INFER_BITSTREAM << std::dec << std::endl;
    }

    // -------------------------------------------------------------------------
    // Allocate input/output/scratch. Fill input with the 16-byte golden vector.
    // -------------------------------------------------------------------------
    pInputBuf  = AlignedAlloc4K(INPUT_SIZE);
    pOutputBuf = AlignedAlloc4K(OUTPUT_SIZE);
    pScratchBuf = (SCRATCH_SIZE > 0) ? AlignedAlloc4K(SCRATCH_SIZE) : nullptr;
    if (!pInputBuf || !pOutputBuf || (SCRATCH_SIZE > 0 && !pScratchBuf)) {
        std::cout << "[main] FAIL: alloc in/out/scratch" << std::endl;
        goto cleanup;
    }

    // Fill input. Padding (if any) stays zero.
    memset(pInputBuf, 0, INPUT_SIZE);
    memcpy(pInputBuf, kGoldenInput, std::min<size_t>(INPUT_SIZE, sizeof(kGoldenInput)));

    // Zero output and scratch so we can tell if the chip actually wrote anything.
    memset(pOutputBuf, 0, OUTPUT_SIZE);
    if (pScratchBuf) memset(pScratchBuf, 0, SCRATCH_SIZE);

    HexDump("input  (golden)", kGoldenInput,  16);
    HexDump("input  (host buf, first 16)", (uint8_t*)pInputBuf, std::min<size_t>(INPUT_SIZE, (size_t)16));

    // -------------------------------------------------------------------------
    // INFER
    // -------------------------------------------------------------------------
    {
        DWORD inferIoctl = useInferWithParam ? IOCTL_INFER_WITH_PARAM : IOCTL_INFER;
        std::cout << "[main] calling " << (useInferWithParam ? "IOCTL_INFER_WITH_PARAM" : "IOCTL_INFER")
                  << "..." << std::endl;

        IOCTL_INFER_INFO ii = {};
        ii.InputImageAddr    = (UINT64)pInputBuf;
        ii.InputImageSize    = INPUT_SIZE;
        ii.OutputBufferAddr  = (UINT64)pOutputBuf;
        ii.OutputBufferSize  = OUTPUT_SIZE;
        ii.InputDeviceVA     = VA_INPUT;
        ii.OutputDeviceVA    = VA_OUTPUT;
        ii.BitstreamDeviceVA = VA_INFER_BITSTREAM;
        ii.BitstreamSize     = model.bitstream.size();
        ii.ScratchAddr       = (UINT64)pScratchBuf;
        ii.ScratchSize       = SCRATCH_SIZE;
        ii.ScratchDeviceVA   = (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0;

        BOOL ok = DeviceIoControl(handle, inferIoctl, &ii, sizeof(ii),
                                  nullptr, 0, &bytesReturned, nullptr);
        if (ok) {
            std::cout << "[main] INFER OK" << std::endl;
        } else {
            std::cout << "[main] INFER failed: " << GetLastError()
                      << " -- output dump still printed below for debugging" << std::endl;
        }
    }

    // -------------------------------------------------------------------------
    // Inspect outfeed
    // -------------------------------------------------------------------------
    {
        const uint8_t* out = (const uint8_t*)pOutputBuf;
        size_t nonZero = 0;
        for (size_t i = 0; i < OUTPUT_SIZE; i++) if (out[i] != 0) nonZero++;

        std::cout << "\n--- output buffer dump ---" << std::endl;
        std::cout << "  total=" << OUTPUT_SIZE << "  non-zero=" << nonZero << std::endl;
        size_t dumpN = std::min<size_t>(OUTPUT_SIZE, (size_t)64);
        std::cout << std::hex << std::setfill('0');
        for (size_t i = 0; i < dumpN; i++) {
            if (i % 16 == 0) std::cout << "  [" << std::setw(4) << i << "] ";
            std::cout << std::setw(2) << (int)out[i] << " ";
            if (i % 16 == 15) std::cout << "\n";
        }
        std::cout << std::dec << std::setfill(' ') << std::endl;

        // Golden compare -- only the first 16 bytes matter for add_int8.
        const size_t cmpN = std::min<size_t>(OUTPUT_SIZE, sizeof(kGoldenOutput));
        bool match = (memcmp(out, kGoldenOutput, cmpN) == 0);

        std::cout << "\n--- golden compare ---" << std::endl;
        HexDump("expected", kGoldenOutput, cmpN);
        HexDump("actual  ", out,           cmpN);

        if (match) {
            std::cout << "  RESULT: PASS -- outfeed bytes match golden" << std::endl;
            DumpRealValues(kGoldenInput, out, cmpN);
            exitCode = 0;
        } else {
            if (nonZero == 0) {
                std::cout << "  RESULT: FAIL -- output buffer is all zero. Chip did not write outfeed." << std::endl;
                std::cout << "          suspect 1) DMA / MMU mapping (output VA -> host PA) broken" << std::endl;
                std::cout << "          suspect 2) chip never executed INFER (e.g. halted right after PARAM_CACHE)" << std::endl;
                std::cout << "          suspect 3) outfeed FIFO drain / cache invalidate missing" << std::endl;
            } else {
                std::cout << "  RESULT: FAIL -- got non-zero data but does not match golden." << std::endl;
                std::cout << "          suspect 1) ADD op / requantize circuitry wrong" << std::endl;
                std::cout << "          suspect 2) infeed picked up wrong bytes (input VA mapping)" << std::endl;
                std::cout << "          suspect 3) cache coherency -- host sees stale data" << std::endl;
                DumpRealValues(kGoldenInput, out, cmpN);
            }
            exitCode = 2;
        }
    }

    // -------------------------------------------------------------------------
    // Unmap INFER bitstream. Param-side stays mapped; the driver releases it
    // during FileCleanup when the device handle is closed.
    // -------------------------------------------------------------------------
    {
        UNMAP_BUFFER_INPUT um = {};
        um.DeviceAddress = VA_INFER_BITSTREAM;
        um.Size          = model.bitstream.size();
        if (DeviceIoControl(handle, IOCTL_UNMAP_BUFFER, &um, sizeof(um),
                            nullptr, 0, &bytesReturned, nullptr)) {
            std::cout << "[main] IOCTL_UNMAP_BUFFER OK" << std::endl;
        } else {
            std::cout << "[main] IOCTL_UNMAP_BUFFER failed: " << GetLastError() << std::endl;
        }
    }

cleanup:
    AlignedFree(pInputBuf);
    AlignedFree(pOutputBuf);
    AlignedFree(pScratchBuf);
    AlignedFree(pInferBitstream);
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    AlignedFree(pParamData);       // safe to free after FileCleanup unlocked the MDLs
    AlignedFree(pParamBitstream);
    AlignedFree(pExe0Phase1Bs);

    std::cout << "\n[main] done. exitCode=" << exitCode << std::endl;
    return exitCode;
}
