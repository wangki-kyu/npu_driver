// npu_runtime.cpp
//
// Implementation of npu_runtime.dll public ABI (see npu_runtime.h).
// Mirrors the init / infer / cleanup flow of infer_test_console.cpp but
// strips all diagnostic dumps so the DLL is usable by GUIs and services.

#include "pch.h"
#include "npu_runtime.h"

#include <objbase.h>   // CoInitializeEx / CoUninitialize (WIN32_LEAN_AND_MEAN drops these)

// objbase.h 는 함수 선언만 줌. import lib (Ole32.lib) 은 명시적으로 link 해야
// MTd (static CRT) 빌드에서도 LNK2019 없이 풀림. /DEFAULTLIB:Ole32.lib 와 동일.
#pragma comment(lib, "Ole32.lib")

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "../include/util.hpp"
#include "../include/Public.h"
#include "../include/apex_model_fb.hpp"
#include "../include/apex_postprocess.hpp"
#include "../include/apex_ssd_postprocess.hpp"

// Static VA layout — copied from infer_test_console.cpp / util.hpp so the
// PTE map matches what the driver expects.
namespace {
constexpr uint64_t VA_INPUT                = 0x01080000ULL;
constexpr uint64_t VA_OUTPUT_BBOX          = 0x01100000ULL;
constexpr uint64_t VA_OUTPUT_SCORE         = 0x01102000ULL;
constexpr uint64_t VA_SCRATCH              = 0x01200000ULL;
constexpr uint64_t VA_EXE1_PARAM_DATA      = 0x00000000ULL;
constexpr uint64_t VA_EXE0_PARAM_DATA      = 0x01040000ULL;
constexpr uint64_t VA_PARAM_BITSTREAM      = 0x00840000ULL;
constexpr uint64_t VA_INFER_BITSTREAM      = 0x00900000ULL;
}  // namespace

struct NpuRuntimeContext {
    apex_fb::ApexModelFb model;
    std::vector<float>   anchors;
    int                  num_anchors = 0;

    // todo: 일단 하드 코딩으로 하나의 모델만 사용 
    apex_pp::QuantParams     q_bbox{ 0.10822763f, 144 };
    apex_pp::QuantParams     q_score{ 0.00390625f, 0 };
    apex_pp::SsdScaleFactors sf{};
    apex_pp::SsdNmsParams    nms{};

    HANDLE hDevice = INVALID_HANDLE_VALUE;
    bool   com_initialized = false;
    bool   has_param_caching = false;

    // Chip-visible user VAs returned by IOCTL_ALLOC_IO_BUFFERS.
    void* pInputBuf       = nullptr;
    void* pOutputBboxBuf  = nullptr;
    void* pOutputScoreBuf = nullptr;
    void* pScratchBuf     = nullptr;
    void* pInferBitstream = nullptr;
    void* pParamData      = nullptr;
    void* pParamBitstream = nullptr;
    void* pExe0ParamData  = nullptr;

    size_t input_size       = 0;
    size_t output_bbox_size = 0;
    size_t output_score_size = 0;
    size_t scratch_size     = 0;

    uint32_t input_w = 0, input_h = 0, input_c = 0;

    // Reusable post-processing scratch (avoid per-infer alloc).
    std::vector<uint8_t> squeeze1_relayout;
    std::vector<uint8_t> scores_relayout;
    std::vector<float>   dq_squeeze1;
    std::vector<float>   dq_scores;
    std::vector<float>   decoded_boxes;

    std::string last_error;
};

namespace {

thread_local std::string g_tls_last_error;

void SetError(NpuRuntimeContext* ctx, const char* msg) {
    if (ctx) ctx->last_error = msg ? msg : "";
    g_tls_last_error = msg ? msg : "";
}

void SetError(NpuRuntimeContext* ctx, const std::string& msg) {
    if (ctx) ctx->last_error = msg;
    g_tls_last_error = msg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public ABI
// ---------------------------------------------------------------------------

extern "C" NPU_API const char* npu_runtime_get_version(void) {
    return "npu_runtime 0.1.0";
}

extern "C" NPU_API const char* npu_runtime_get_last_error(npu_handle_t h) {
    if (h && !h->last_error.empty()) return h->last_error.c_str();
    return g_tls_last_error.c_str();
}

extern "C" NPU_API npu_status_t npu_runtime_init_face_ssd(
    const char*   model_path_utf8,
    const char*   anchors_bin_path_utf8,
    npu_handle_t* out_handle)
{
    if (!model_path_utf8 || !anchors_bin_path_utf8 || !out_handle) {
        SetError(nullptr, "invalid arg: null pointer");
        return NPU_ERR_INVALID_ARG;
    }
    *out_handle = nullptr;

    NpuRuntimeContext* ctx = nullptr;
    try {
        ctx = new (std::nothrow) NpuRuntimeContext();
        if (!ctx) {
            SetError(nullptr, "context alloc failed");
            return NPU_ERR_ALLOC_FAIL;
        }

        // 1. COM (WIC is not used here, but harmless to init for callers).
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            ctx->com_initialized = SUCCEEDED(hr);
        }

        // 2. Load model.
        ctx->model = apex_fb::LoadModel(model_path_utf8);
        if (ctx->model.bitstream.empty() ||
            ctx->model.input_layers.empty() ||
            ctx->model.output_layers.empty()) {
            SetError(ctx, std::string("model load failed: ") + model_path_utf8);
            npu_runtime_free(ctx);
            return NPU_ERR_MODEL_LOAD_FAIL;
        }
        ctx->has_param_caching =
            !ctx->model.param_bitstream.empty() && !ctx->model.parameters.empty();

        // 3. Load anchors.
        ctx->anchors = apex_pp::LoadAnchorsBin(anchors_bin_path_utf8);
        if (ctx->anchors.empty() || (ctx->anchors.size() % 4) != 0) {
            SetError(ctx, std::string("anchors load failed: ") + anchors_bin_path_utf8);
            npu_runtime_free(ctx);
            return NPU_ERR_ANCHORS_LOAD_FAIL;
        }
        ctx->num_anchors = static_cast<int>(ctx->anchors.size() / 4);

        // 4. Cache input dims.
        const auto& in0 = ctx->model.input_layers[0];
        ctx->input_size = in0.size_bytes;
        ctx->input_w    = static_cast<uint32_t>(in0.x_dim);
        ctx->input_h    = static_cast<uint32_t>(in0.y_dim);
        ctx->input_c    = static_cast<uint32_t>(in0.z_dim);

        ctx->output_bbox_size  = (ctx->model.output_layers.size() >= 1)
            ? ctx->model.output_layers[0].size_bytes : 0x2000;
        ctx->output_score_size = (ctx->model.output_layers.size() >= 2)
            ? ctx->model.output_layers[1].size_bytes : 0x2000;
        ctx->scratch_size = ctx->model.scratch_size_bytes;

        // 5. Open device.
        ctx->hDevice = FindPicoDriverDevice(GUID_DEVINTERFACE_npudriver);
        if (ctx->hDevice == nullptr || ctx->hDevice == INVALID_HANDLE_VALUE) {
            SetError(ctx, "device handle: npu driver not found");
            npu_runtime_free(ctx);
            return NPU_ERR_DEVICE_NOT_FOUND;
        }

        // 6. Allocate all I/O buffers in one IOCTL.
        IOCTL_ALLOC_IO_BUFFERS_IN  in{};
        IOCTL_ALLOC_IO_BUFFERS_OUT out{};
        in.InputSize             = ctx->input_size;
        in.InputDeviceVA         = VA_INPUT;
        in.OutputBboxSize        = ctx->output_bbox_size;
        in.OutputBboxDeviceVA    = VA_OUTPUT_BBOX;
        in.OutputScoreSize       = ctx->output_score_size;
        in.OutputScoreDeviceVA   = VA_OUTPUT_SCORE;
        in.ScratchSize           = ctx->scratch_size;
        in.ScratchDeviceVA       = (ctx->scratch_size > 0) ? VA_SCRATCH : 0;
        in.Exe0BitstreamSize     = ctx->model.bitstream.size();
        in.Exe0BitstreamDeviceVA = VA_INFER_BITSTREAM;

        if (ctx->has_param_caching) {
            in.ParamDataSize         = ctx->model.parameters.size();
            in.ParamDataDeviceVA     = VA_EXE1_PARAM_DATA;
            in.Exe1BitstreamSize     = ctx->model.param_bitstream.size();
            in.Exe1BitstreamDeviceVA = VA_PARAM_BITSTREAM;
            in.Exe0ParamSize         = ctx->model.exe0_parameters.size();
            in.Exe0ParamDeviceVA     = VA_EXE0_PARAM_DATA;
        }

        DWORD br = 0;
        if (!DeviceIoControl(ctx->hDevice, IOCTL_ALLOC_IO_BUFFERS,
                             &in, sizeof(in), &out, sizeof(out), &br, nullptr)) {
            SetError(ctx, std::string("IOCTL_ALLOC_IO_BUFFERS failed, GetLastError=") +
                          std::to_string(GetLastError()));
            npu_runtime_free(ctx);
            return NPU_ERR_IOCTL_FAIL;
        }

        ctx->pInputBuf       = reinterpret_cast<void*>(out.InputUserVA);
        ctx->pOutputBboxBuf  = reinterpret_cast<void*>(out.OutputBboxUserVA);
        ctx->pOutputScoreBuf = reinterpret_cast<void*>(out.OutputScoreUserVA);
        ctx->pScratchBuf     = (ctx->scratch_size > 0)
            ? reinterpret_cast<void*>(out.ScratchUserVA) : nullptr;
        ctx->pInferBitstream = reinterpret_cast<void*>(out.Exe0BitStreamUserVA);

        if (ctx->has_param_caching) {
            ctx->pParamData      = reinterpret_cast<void*>(out.ParamDataUserVA);
            ctx->pParamBitstream = reinterpret_cast<void*>(out.Exe1BitstreamUserVA);
            ctx->pExe0ParamData  = reinterpret_cast<void*>(out.Exe0ParamUserVA);

            // 7. Copy parameter blobs into the chip-visible slots.
            std::memcpy(ctx->pParamData,
                        ctx->model.parameters.data(),
                        ctx->model.parameters.size());
            std::memcpy(ctx->pExe0ParamData,
                        ctx->model.exe0_parameters.data(),
                        ctx->model.exe0_parameters.size());

            // 8. Patch exe1 (PARAM_CACHING) bitstream with PARAM device VA,
            //    then copy into chip-visible slot.
            apex_fb::PatchParamBitstreamVAs(ctx->model, VA_EXE1_PARAM_DATA);
            std::memcpy(ctx->pParamBitstream,
                        ctx->model.param_bitstream.data(),
                        ctx->model.param_bitstream.size());
        }

        // 9. Patch exe0 (main inference) bitstream with all base VAs.
        std::map<std::string, uint64_t> output_vas;
        if (ctx->model.output_layers.size() >= 1)
            output_vas[ctx->model.output_layers[0].name] = VA_OUTPUT_BBOX;
        if (ctx->model.output_layers.size() >= 2)
            output_vas[ctx->model.output_layers[1].name] = VA_OUTPUT_SCORE;

        apex_fb::PatchVAs(ctx->model,
                          VA_INPUT,
                          output_vas,
                          ctx->has_param_caching ? VA_EXE0_PARAM_DATA : 0,
                          (ctx->scratch_size > 0) ? VA_SCRATCH : 0);

        std::memcpy(ctx->pInferBitstream,
                    ctx->model.bitstream.data(),
                    ctx->model.bitstream.size());

        // 10. Pre-allocate post-processing scratch.
        if (ctx->model.output_layers.size() >= 1) {
            ctx->squeeze1_relayout.resize(
                apex_pp::ActualSizeBytes(ctx->model.output_layers[0]));
        }
        if (ctx->model.output_layers.size() >= 2) {
            ctx->scores_relayout.resize(
                apex_pp::ActualSizeBytes(ctx->model.output_layers[1]));
        }
        ctx->dq_squeeze1.resize(static_cast<size_t>(ctx->num_anchors) * 4);
        ctx->dq_scores.resize(static_cast<size_t>(ctx->num_anchors) *
                              (ctx->nms.num_classes + 1));
        ctx->decoded_boxes.resize(static_cast<size_t>(ctx->num_anchors) * 4);

        *out_handle = ctx;
        return NPU_OK;
    }
    catch (const std::exception& e) {
        SetError(ctx, std::string("init exception: ") + e.what());
        if (ctx) npu_runtime_free(ctx);
        return NPU_ERR_INTERNAL;
    }
    catch (...) {
        SetError(ctx, "init exception: unknown");
        if (ctx) npu_runtime_free(ctx);
        return NPU_ERR_INTERNAL;
    }
}

extern "C" NPU_API npu_status_t npu_runtime_get_input_size(
    npu_handle_t h, uint32_t* out_w, uint32_t* out_h, uint32_t* out_c)
{
    if (!h || !out_w || !out_h || !out_c) return NPU_ERR_INVALID_ARG;
    *out_w = h->input_w;
    *out_h = h->input_h;
    *out_c = h->input_c;
    return NPU_OK;
}

extern "C" NPU_API npu_status_t npu_runtime_infer_image(
    npu_handle_t     h,
    const uint8_t*   image_rgb,
    uint32_t         image_byte_len,
    npu_detection_t* out_dets,
    uint32_t         max,
    uint32_t*        out_count)
{
    if (!h || !image_rgb || !out_count) {
        SetError(h, "infer: invalid arg");
        return NPU_ERR_INVALID_ARG;
    }
    *out_count = 0;

    if (image_byte_len != h->input_size) {
        SetError(h, std::string("infer: image size mismatch, got ") +
                    std::to_string(image_byte_len) + " expected " +
                    std::to_string(h->input_size));
        return NPU_ERR_INVALID_ARG;
    }
    if (max > 0 && !out_dets) {
        SetError(h, "infer: out_dets is null but max > 0");
        return NPU_ERR_INVALID_ARG;
    }

    try {
        // 1. Copy caller input into chip-visible input slot.
        std::memcpy(h->pInputBuf, image_rgb, h->input_size);

        // 2. Fire IOCTL_INFER_NEW.
        IOCTL_INFER_INFO ii{};
        ii.InputImageAddr      = reinterpret_cast<UINT64>(h->pInputBuf);
        ii.InputImageSize      = h->input_size;
        ii.OutputBboxAddr      = reinterpret_cast<UINT64>(h->pOutputBboxBuf);
        ii.OutputBboxSize      = h->output_bbox_size;
        ii.OutputBboxDeviceVA  = VA_OUTPUT_BBOX;
        ii.OutputScoreAddr     = reinterpret_cast<UINT64>(h->pOutputScoreBuf);
        ii.OutputScoreSize     = h->output_score_size;
        ii.OutputScoreDeviceVA = VA_OUTPUT_SCORE;
        ii.InputDeviceVA       = VA_INPUT;
        ii.BitstreamDeviceVA   = VA_INFER_BITSTREAM;
        ii.BitstreamSize       = h->model.bitstream.size();
        ii.ScratchAddr         = reinterpret_cast<UINT64>(h->pScratchBuf);
        ii.ScratchSize         = h->scratch_size;
        ii.ScratchDeviceVA     = (h->scratch_size > 0) ? VA_SCRATCH : 0;

        DWORD br = 0;
        if (!DeviceIoControl(h->hDevice, IOCTL_INFER_NEW, &ii, sizeof(ii),
                             nullptr, 0, &br, nullptr)) {
            SetError(h, std::string("IOCTL_INFER_NEW failed, GetLastError=") +
                        std::to_string(GetLastError()));
            return NPU_ERR_INFER_FAIL;
        }

        // 3. Relayout + signed transform (chip TYXZ tile-interleaved -> linear YXZ).
        if (h->model.output_layers.size() < 2) {
            SetError(h, "infer: model has < 2 output layers (face SSD expects 2)");
            return NPU_ERR_INFER_FAIL;
        }
        apex_pp::RelayoutAndSignedXform(
            h->squeeze1_relayout.data(),
            static_cast<const uint8_t*>(h->pOutputBboxBuf),
            h->model.output_layers[0]);
        apex_pp::RelayoutAndSignedXform(
            h->scores_relayout.data(),
            static_cast<const uint8_t*>(h->pOutputScoreBuf),
            h->model.output_layers[1]);

        // 4. Sanity check buffer sizes vs num_anchors.
        const int num_anchors      = h->num_anchors;
        const size_t exp_squeeze1  = static_cast<size_t>(num_anchors) * 4;
        const size_t exp_scores    = static_cast<size_t>(num_anchors) *
                                     (h->nms.num_classes + 1);
        if (h->squeeze1_relayout.size() != exp_squeeze1 ||
            h->scores_relayout.size()   != exp_scores) {
            SetError(h, "infer: relayout size mismatch vs num_anchors");
            return NPU_ERR_INFER_FAIL;
        }

        // 5. Dequantize.
        apex_pp::Dequantize(h->dq_squeeze1.data(),
                            h->squeeze1_relayout.data(),
                            h->dq_squeeze1.size(),
                            h->q_bbox);
        apex_pp::Dequantize(h->dq_scores.data(),
                            h->scores_relayout.data(),
                            h->dq_scores.size(),
                            h->q_score);

        // 6. DecodeBoxes (Squeeze1 + anchors -> ymin/xmin/ymax/xmax).
        apex_pp::DecodeBoxes(h->decoded_boxes.data(),
                             h->dq_squeeze1.data(),
                             h->anchors.data(),
                             num_anchors,
                             h->sf);

        // 7. NMS.
        std::vector<apex_pp::Detection> dets = apex_pp::NonMaxSuppression(
            h->decoded_boxes.data(),
            h->dq_scores.data(),
            num_anchors,
            h->nms);

        // 8. Copy out to caller buffer.
        const uint32_t n = (max < dets.size())
            ? max : static_cast<uint32_t>(dets.size());
        for (uint32_t i = 0; i < n; ++i) {
            out_dets[i].class_id = dets[i].class_id;
            out_dets[i].score    = dets[i].score;
            out_dets[i].ymin     = dets[i].ymin;
            out_dets[i].xmin     = dets[i].xmin;
            out_dets[i].ymax     = dets[i].ymax;
            out_dets[i].xmax     = dets[i].xmax;
        }
        *out_count = n;
        return NPU_OK;
    }
    catch (const std::exception& e) {
        SetError(h, std::string("infer exception: ") + e.what());
        return NPU_ERR_INTERNAL;
    }
    catch (...) {
        SetError(h, "infer exception: unknown");
        return NPU_ERR_INTERNAL;
    }
}

extern "C" NPU_API void npu_runtime_free(npu_handle_t h) {
    if (!h) return;
    try {
        if (h->hDevice && h->hDevice != INVALID_HANDLE_VALUE) {
            DWORD br = 0;
            DeviceIoControl(h->hDevice, IOCTL_FREE_IO_BUFFERS,
                            nullptr, 0, nullptr, 0, &br, nullptr);
            CloseHandle(h->hDevice);
            h->hDevice = INVALID_HANDLE_VALUE;
        }
        if (h->com_initialized) {
            CoUninitialize();
            h->com_initialized = false;
        }
    }
    catch (...) {
        // swallow — destructor must not throw across C ABI boundary
    }
    delete h;
}
