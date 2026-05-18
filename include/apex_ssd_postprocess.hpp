// apex_ssd_postprocess.hpp
//
// SSD detection post-processing for Coral SSD MobileNet (face) models.
// Standalone: no pycoral / no libedgetpu runtime / no tflite_runtime dependency.
//
// Pipeline (called after apex_pp::RelayoutAndSignedXform):
//   1. Dequantize uint8 -> float32  (per-tensor quant: scale * (x - zp))
//   2. DecodeBoxes : Squeeze1 (dy,dx,dh,dw) + anchors -> bbox (ymin,xmin,ymax,xmax)
//   3. NonMaxSuppression : threshold by score, suppress overlapping boxes
//
// Anchors layout (per TFLite SSD convention): [ycenter, xcenter, h, w] normalized [0..1].
// Squeeze1 4-byte per anchor after dequant: [dy, dx, dh, dw].
// convert_scores 2-byte per anchor after dequant: [bg_score, face_score] (for face model).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace apex_pp {

// Per-tensor uniform quantization parameters (matches tflite).
struct QuantParams {
    float scale       = 1.0f;
    int   zero_point  = 0;
};

// SSD box-encoding scale factors. TFLite_Detection_PostProcess defaults
// for SSD MobileNet are (y=10, x=10, h=5, w=5). Override if model differs.
struct SsdScaleFactors {
    float y_scale = 10.0f;
    float x_scale = 10.0f;
    float h_scale = 5.0f;
    float w_scale = 5.0f;
};

// NMS configuration.
struct SsdNmsParams {
    float score_threshold = 0.5f;   // detect.py default for face
    float iou_threshold   = 0.6f;   // TFLite_Detection_PostProcess default
    int   max_detections  = 50;
    // num_classes excludes the background class. Number of class scores
    // per anchor in convert_scores is (1 + num_classes).
    int   num_classes     = 1;      // face only
};

// One detection result (normalized [0..1] coords; multiply by input W/H for pixels).
struct Detection {
    float score;
    int   class_id;     // 0..num_classes-1
    float ymin, xmin, ymax, xmax;
};

// --- Dequantize uint8 -> float32 ---
inline void Dequantize(float* dst, const uint8_t* src, size_t count,
                       const QuantParams& q) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = (static_cast<int>(src[i]) - q.zero_point) * q.scale;
    }
}

// --- DecodeBoxes : Squeeze (dy,dx,dh,dw) + anchors -> (ymin,xmin,ymax,xmax) ---
//
// anchors : [num_anchors][4] = (ycenter, xcenter, h, w)
// encoded : [num_anchors][4] = (dy, dx, dh, dw)   (already dequantized)
// out     : [num_anchors][4] = (ymin, xmin, ymax, xmax)
inline void DecodeBoxes(float* out_bboxes,
                        const float* encoded,
                        const float* anchors,
                        int num_anchors,
                        const SsdScaleFactors& sf = {}) {
    for (int i = 0; i < num_anchors; ++i) {
        const float dy = encoded[i * 4 + 0];
        const float dx = encoded[i * 4 + 1];
        const float dh = encoded[i * 4 + 2];
        const float dw = encoded[i * 4 + 3];

        const float ay = anchors[i * 4 + 0];
        const float ax = anchors[i * 4 + 1];
        const float ah = anchors[i * 4 + 2];
        const float aw = anchors[i * 4 + 3];

        const float ycenter = ay + (dy / sf.y_scale) * ah;
        const float xcenter = ax + (dx / sf.x_scale) * aw;
        const float h       = std::exp(dh / sf.h_scale) * ah;
        const float w       = std::exp(dw / sf.w_scale) * aw;

        out_bboxes[i * 4 + 0] = ycenter - h * 0.5f;   // ymin
        out_bboxes[i * 4 + 1] = xcenter - w * 0.5f;   // xmin
        out_bboxes[i * 4 + 2] = ycenter + h * 0.5f;   // ymax
        out_bboxes[i * 4 + 3] = xcenter + w * 0.5f;   // xmax
    }
}

// --- IoU between two boxes (ymin, xmin, ymax, xmax) ---
inline float IoU(const float* a, const float* b) {
    const float iy1 = (a[0] > b[0]) ? a[0] : b[0];
    const float ix1 = (a[1] > b[1]) ? a[1] : b[1];
    const float iy2 = (a[2] < b[2]) ? a[2] : b[2];
    const float ix2 = (a[3] < b[3]) ? a[3] : b[3];
    const float iw = (ix2 > ix1) ? (ix2 - ix1) : 0.0f;
    const float ih = (iy2 > iy1) ? (iy2 - iy1) : 0.0f;
    const float inter = iw * ih;
    const float aw = (a[3] - a[1]); const float ah = (a[2] - a[0]);
    const float bw = (b[3] - b[1]); const float bh = (b[2] - b[0]);
    const float a_area = (aw > 0 && ah > 0) ? (aw * ah) : 0.0f;
    const float b_area = (bw > 0 && bh > 0) ? (bw * bh) : 0.0f;
    const float uni = a_area + b_area - inter;
    return (uni > 0.0f) ? (inter / uni) : 0.0f;
}

// --- NMS over per-class scores ---
//
// scores : [num_anchors][num_classes + 1]   (index 0 = background, skipped)
// bboxes : [num_anchors][4]                 (ymin, xmin, ymax, xmax, normalized)
inline std::vector<Detection> NonMaxSuppression(
        const float* bboxes,
        const float* scores,
        int num_anchors,
        const SsdNmsParams& p) {
    const int num_class_slots = p.num_classes + 1;  // includes background slot 0
    std::vector<Detection> cand;
    cand.reserve(64);

    for (int i = 0; i < num_anchors; ++i) {
        // Object classes only (skip background = slot 0).
        for (int c = 1; c < num_class_slots; ++c) {
            const float s = scores[i * num_class_slots + c];
            if (s < p.score_threshold) continue;
            Detection d;
            d.score    = s;
            d.class_id = c - 1;  // 0-indexed object class
            d.ymin = bboxes[i * 4 + 0];
            d.xmin = bboxes[i * 4 + 1];
            d.ymax = bboxes[i * 4 + 2];
            d.xmax = bboxes[i * 4 + 3];
            cand.push_back(d);
        }
    }

    // Sort by score descending.
    std::sort(cand.begin(), cand.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });

    std::vector<Detection> kept;
    std::vector<bool> sup(cand.size(), false);
    for (size_t i = 0; i < cand.size(); ++i) {
        if (sup[i]) continue;
        kept.push_back(cand[i]);
        if (static_cast<int>(kept.size()) >= p.max_detections) break;
        for (size_t j = i + 1; j < cand.size(); ++j) {
            if (sup[j]) continue;
            if (cand[i].class_id != cand[j].class_id) continue;
            if (IoU(&cand[i].ymin, &cand[j].ymin) > p.iou_threshold) {
                sup[j] = true;
            }
        }
    }
    return kept;
}

// --- Convenience: load anchors from a binary file (float32 little-endian) ---
inline std::vector<float> LoadAnchorsBin(const std::string& path) {
    std::vector<float> data;
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && (sz % sizeof(float) == 0)) {
        data.resize(static_cast<size_t>(sz) / sizeof(float));
        fread(data.data(), 1, sz, f);
    }
    fclose(f);
    return data;
}

// --- End-to-end pipeline (Squeeze1 + convert_scores uint8 buffers -> detections) ---
//
// squeeze1_uint8     : [num_anchors][4] uint8  (post-Relayout)
// scores_uint8       : [num_anchors][2] uint8  (post-Relayout, [bg, face] for face model)
// anchors            : [num_anchors][4] float32 (loaded from anchors.bin)
// returns sorted-by-score detections (normalized coords).
inline std::vector<Detection> RunSsdPostprocess(
        const uint8_t* squeeze1_uint8,
        const uint8_t* scores_uint8,
        int num_anchors,
        const float* anchors,
        const QuantParams& squeeze1_q,
        const QuantParams& scores_q,
        const SsdScaleFactors& sf = {},
        const SsdNmsParams& nms = {}) {
    const int num_class_slots = nms.num_classes + 1;

    std::vector<float> dq_boxes(num_anchors * 4);
    std::vector<float> dq_scores(num_anchors * num_class_slots);
    std::vector<float> decoded(num_anchors * 4);

    Dequantize(dq_boxes.data(),  squeeze1_uint8, dq_boxes.size(),  squeeze1_q);
    Dequantize(dq_scores.data(), scores_uint8,   dq_scores.size(), scores_q);

    DecodeBoxes(decoded.data(), dq_boxes.data(), anchors, num_anchors, sf);

    return NonMaxSuppression(decoded.data(), dq_scores.data(), num_anchors, nms);
}

}  // namespace apex_pp
