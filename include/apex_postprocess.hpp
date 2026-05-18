// apex_postprocess.hpp
//
// Pure-host post-processing for chip OUTFEED bytes (no pycoral, no libedgetpu
// runtime dependency). Mirrors libedgetpu's
//   api/layer_information.cc :: OutputLayerInformation::Relayout()
//   api/layer_information.cc :: LayerInformation::TransformSignedDataType()
// but takes our LayerInfo (apex_model_fb.hpp) instead of the libedgetpu
// LayerInformation wrapper.
//
// The chip writes OUTFEED in TYXZ tile-interleaved padded layout. This file
// converts that into linear YXZ uint8/uint16 buffer that downstream code
// (Dequantize, SSD decode, NMS) can consume directly.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "apex_model_fb.hpp"

namespace apex_pp {

// --- DataType helpers (mirrors layer_information.cc:494 TensorDataTypeSize) ---
inline int DataTypeSize(platforms::darwinn::DataType dt) {
    using namespace platforms::darwinn;
    switch (dt) {
        case DataType_FIXED_POINT8:
        case DataType_SIGNED_FIXED_POINT8:  return 1;
        case DataType_FIXED_POINT16:
        case DataType_SIGNED_FIXED_POINT16: return 2;
        case DataType_SIGNED_FIXED_POINT32: return 4;
        case DataType_BFLOAT:               return 2;
        case DataType_HALF:                 return 2;
        case DataType_SINGLE:               return 4;
        default:                            return 1;
    }
}

inline bool IsSignedDataType(platforms::darwinn::DataType dt) {
    using namespace platforms::darwinn;
    return dt == DataType_SIGNED_FIXED_POINT8 ||
           dt == DataType_SIGNED_FIXED_POINT16;
}

// dst size (post-Relayout) = y_dim * x_dim * z_dim * DataTypeSize(dt)
// src size (chip OUTFEED raw) = layer.size_bytes (per iteration, may include padding)
inline size_t ActualSizeBytes(const apex_fb::LayerInfo& layer) {
    return (size_t)layer.y_dim * layer.x_dim * layer.z_dim
         * DataTypeSize(layer.data_type)
         * (layer.execution_count_per_inference > 0 ? layer.execution_count_per_inference : 1);
}

// --- Relayout (port of layer_information.cc:206-376) ---
//
// Maps chip's TYXZ tile-interleaved padded buffer to linear YXZ buffer.
// dst must be at least ActualSizeBytes(layer) bytes.
// src must be at least layer.size_bytes * execution_count bytes (chip raw).
//
// Only the single-execution path (execution_count_per_inference == 1) is
// supported here — the Coral models we target (SSD MobileNet face, etc.) all
// use exec=1. Multi-execution support can be added if ever needed.
inline void Relayout(uint8_t* dst, const uint8_t* src, const apex_fb::LayerInfo& layer) {
    const int data_type_size = DataTypeSize(layer.data_type);
    const int z_bytes        = layer.z_dim * data_type_size;
    const int executions     = layer.execution_count_per_inference > 0
                             ? layer.execution_count_per_inference : 1;

    // No layout info OR no relayout needed: plain memcpy.
    if (!layer.has_layout) {
        std::memcpy(dst, src,
                    (size_t)layer.y_dim * layer.x_dim * z_bytes * executions);
        return;
    }

    // We currently only handle single execution; multi-execution would need
    // per-iteration src/dst advance like libedgetpu does.
    (void)executions;

    // GetBufferIndex (layer_information.cc:171-191).
    // Returns element index (NOT byte index) — caller multiplies by data_type_size.
    auto get_buf_idx = [&](int y, int x, int z) -> int {
        const int tile_id =
            layer.y_to_tile_id[y] + layer.x_to_tile_id[x];
        const int global_tile_byte_offset =
            layer.tile_byte_offset[tile_id];
        const int local_x_byte_offset =
            layer.x_to_local_byte[x];
        const int local_y_byte_offset =
            layer.y_to_local_y_offset[y] * layer.x_to_local_y_row_size[x];
        return global_tile_byte_offset + local_y_byte_offset
             + local_x_byte_offset + z;
    };

    // Derive padded z bytes (layer_information.cc:258-268).
    int z_bytes_padded;
    if (layer.x_dim > 1) {
        z_bytes_padded = get_buf_idx(0, 1, 0) - get_buf_idx(0, 0, 0);
    } else {
        z_bytes_padded = get_buf_idx(1, 0, 0) - get_buf_idx(0, 0, 0);
    }
    z_bytes_padded *= data_type_size;

    // Build active_tile_x_sizes (layer_information.cc:271-282).
    std::vector<int> active_tile_x_sizes;
    int last_x = 0;
    int last_x_tile = layer.x_to_tile_id[0];
    for (int x = 1; x < layer.x_dim; ++x) {
        const int cur_x_tile = layer.x_to_tile_id[x];
        if (cur_x_tile != last_x_tile) {
            active_tile_x_sizes.push_back(x - last_x);
            last_x_tile = cur_x_tile;
            last_x      = x;
        }
    }
    active_tile_x_sizes.push_back(layer.x_dim - last_x);

    // Walk (y, x_tile, local_x) and copy one z-vector per element.
    // Generalized form of RELAYOUT_WITH_Z_BYTES_SPECIALIZATION
    // (layer_information.cc:292-319).
    for (int y = 0; y < layer.y_dim; ++y) {
        int tile_starting_x = 0;
        for (size_t xt = 0; xt < active_tile_x_sizes.size(); ++xt) {
            const uint8_t* source =
                src + (size_t)get_buf_idx(y, tile_starting_x, 0) * data_type_size;
            const int tile_x_size = active_tile_x_sizes[xt];
            for (int lx = 0; lx < tile_x_size; ++lx) {
                std::memcpy(dst, source, z_bytes);
                dst   += z_bytes;
                source += z_bytes_padded;
            }
            tile_starting_x += tile_x_size;
        }
    }
}

// --- TransformSignedDataType (port of layer_information.cc:130-149) ---
//
// For SIGNED_FIXED_POINT8/16 layers, flips MSB of every element so that
// unsigned chip bytes become int8/int16. No-op for unsigned layers.
inline void TransformSignedDataType(uint8_t* buffer, size_t total_bytes,
                                    const apex_fb::LayerInfo& layer) {
    if (!IsSignedDataType(layer.data_type)) return;
    const int dts = DataTypeSize(layer.data_type);
    if (dts <= 0) return;
    // XOR the MSB byte of each element with 0x80.
    for (size_t i = (size_t)(dts - 1); i < total_bytes; i += (size_t)dts) {
        buffer[i] ^= 0x80;
    }
}

// Convenience: do Relayout into `dst`, then in-place SignedDataType flip.
// Returns the number of bytes written into dst (== ActualSizeBytes(layer)).
inline size_t RelayoutAndSignedXform(uint8_t* dst, const uint8_t* src,
                                     const apex_fb::LayerInfo& layer) {
    Relayout(dst, src, layer);
    const size_t n = ActualSizeBytes(layer);
    TransformSignedDataType(dst, n, layer);
    return n;
}

}  // namespace apex_pp
