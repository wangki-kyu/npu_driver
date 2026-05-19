// npu_runtime.h
//
// Public C ABI for npu_runtime.dll — wraps the npu_driver IOCTL flow
// (model load + buffer alloc + chip inference + SSD post-processing) so any
// language (Python ctypes, C# DllImport, Rust FFI, C/C++ LoadLibrary) can call
// the same entry points.
//
// MVP scope (v0.1):
//   - Single model family: SSD MobileNet v2 face (320x320 RGB uint8 input)
//   - Caller provides model_path (.tflite) and anchors_path (.bin)
//   - Caller provides input as raw RGB uint8 packed (320*320*3 = 307200 bytes)
//   - Returns up to `max` detections in caller-allocated buffer
//
// Calling convention: __cdecl (default for all listed languages).
// String encoding:    UTF-8.
// Threading:          Each handle is single-thread for v0.1 (no internal lock).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NPURUNTIME_EXPORTS
#  define NPU_API __declspec(dllexport)
#else
#  define NPU_API __declspec(dllimport)
#endif

typedef struct NpuRuntimeContext* npu_handle_t;

typedef enum {
    NPU_OK                    = 0,
    NPU_ERR_INVALID_ARG       = 1,
    NPU_ERR_DEVICE_NOT_FOUND  = 2,
    NPU_ERR_MODEL_LOAD_FAIL   = 3,
    NPU_ERR_ALLOC_FAIL        = 4,
    NPU_ERR_IOCTL_FAIL        = 5,
    NPU_ERR_INFER_FAIL        = 6,
    NPU_ERR_ANCHORS_LOAD_FAIL = 7,
    NPU_ERR_INTERNAL          = 99
} npu_status_t;

typedef struct {
    int32_t class_id;
    float   score;
    float   ymin;
    float   xmin;
    float   ymax;
    float   xmax;
} npu_detection_t;

// Version string ("npu_runtime <major>.<minor>.<patch>"). Always non-NULL.
NPU_API const char* npu_runtime_get_version(void);

// Last error message for a given handle. If h is NULL, returns the
// thread_local fallback set by init failures (before any handle exists).
// Returned pointer is owned by the runtime; do not free.
NPU_API const char* npu_runtime_get_last_error(npu_handle_t h);

// Initialize a runtime instance for the face SSD model.
//   model_path_utf8   : UTF-8 path to ssd_mobilenet_v2_face_*.tflite
//   anchors_bin_path_utf8 : UTF-8 path to anchors.bin (float32 [num_anchors][4])
//   out_handle        : on success, receives a non-NULL handle
NPU_API npu_status_t npu_runtime_init_face_ssd(
    const char*   model_path_utf8,
    const char*   anchors_bin_path_utf8,
    npu_handle_t* out_handle);

// Query model input dimensions (w, h, c). Bytes = w * h * c.
NPU_API npu_status_t npu_runtime_get_input_size(
    npu_handle_t h,
    uint32_t* out_w,
    uint32_t* out_h,
    uint32_t* out_c);

// Run one inference.
//   image_rgb       : RGB uint8 packed, must be exactly w*h*c bytes
//   image_byte_len  : length of image_rgb (sanity check)
//   out_dets        : caller-allocated array of `max` entries (may be NULL if max==0)
//   max             : capacity of out_dets
//   out_count       : actually written (always <= max)
NPU_API npu_status_t npu_runtime_infer_image(
    npu_handle_t     h,
    const uint8_t*   image_rgb,
    uint32_t         image_byte_len,
    npu_detection_t* out_dets,
    uint32_t         max,
    uint32_t*        out_count);

// Release all resources held by the handle (chip buffers, device handle, COM).
// Safe to call with NULL.
NPU_API void npu_runtime_free(npu_handle_t h);

#ifdef __cplusplus
}  // extern "C"
#endif
