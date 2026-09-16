/// @file ScrfdDecode.cpp
/// @brief SCRFD 锚点解码 + Greedy NMS
///
/// 算法直接移植自:
///   - test/Neel_cpp/trt_face.cpp:620-698 (检测解码)
///   - test/Neel_cpp/trt_face.cpp:213-243 (NMS)

#include "FaceTypes.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace face_infer {

// ============================================================
// IoU 计算 (trt_face.cpp:213-221)
// ============================================================
static float iou(const FaceDetection& a, const FaceDetection& b) {
    float ix1 = std::max(a.x1, b.x1);
    float iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2);
    float iy2 = std::min(a.y2, b.y2);
    float inter = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

// ============================================================
// Greedy NMS (trt_face.cpp:223-243)
// ============================================================
static void nms(std::vector<FaceDetection>& dets, float thresh) {
    std::sort(dets.begin(), dets.end(),
              [](const FaceDetection& a, const FaceDetection& b) {
                  return a.score > b.score;
              });

    std::vector<bool> suppressed(dets.size(), false);
    std::vector<FaceDetection> result;
    result.reserve(dets.size());

    for (size_t i = 0; i < dets.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!suppressed[j] && iou(dets[i], dets[j]) > thresh)
                suppressed[j] = true;
        }
    }
    dets = std::move(result);
}

// ============================================================
// SCRFD 输出解码 (trt_face.cpp:620-698)
// ============================================================
int ScrfdDecode(const float* const* output_tensors,
                float scale, int orig_w, int orig_h,
                float conf_thresh, float nms_thresh,
                FaceDetection* out_dets, int max_dets)
{
    std::vector<FaceDetection> dets;
    dets.reserve(256);

    for (int s = 0; s < kScrfdNumStrides; s++) {
        int stride = kScrfdStrides[s];
        int feat_w = kScrfdInputSize / stride;  // 80, 40, 20
        int n_anchors = ScrfdTensorMap::anchor_count[s];  // 12800, 3200, 800
        int anchors_per_point = kScrfdFmc;                // 2

        const float* scores = output_tensors[ScrfdTensorMap::score_idx[s]];
        const float* bboxes = output_tensors[ScrfdTensorMap::bbox_idx[s]];
        const float* kps    = output_tensors[ScrfdTensorMap::kps_idx[s]];

        for (int i = 0; i < n_anchors; i++) {
            float score = scores[i];

            // 自适应 sigmoid (trt_face.cpp:638-644)
            // SCRFD 输出可能是 raw logit 或 post-sigmoid
            // 值域超出 [0,1] 则应用 sigmoid
            if (score > 1.0f || score < 0.0f) {
                if (score < -10.0f) continue;
                score = 1.0f / (1.0f + std::exp(-score));
            }

            if (score < conf_thresh) continue;

            // 锚点位置 (trt_face.cpp:648-653)
            int point_idx = i / anchors_per_point;
            int ax = point_idx % feat_w;
            int ay = point_idx / feat_w;
            float anchor_cx = ax * stride;
            float anchor_cy = ay * stride;

            // distance bbox 解码 (trt_face.cpp:656-660)
            const float* bbox = &bboxes[i * 4];
            float x1 = (anchor_cx - bbox[0] * stride) * scale;
            float y1 = (anchor_cy - bbox[1] * stride) * scale;
            float x2 = (anchor_cx + bbox[2] * stride) * scale;
            float y2 = (anchor_cy + bbox[3] * stride) * scale;

            // landmark 解码 (trt_face.cpp:673-676)
            const float* lm = &kps[i * 10];

            FaceDetection det;
            det.score = score;

            // letterbox 逆映射 (trt_face.cpp:667-671)
            det.x1 = x1;
            det.y1 = y1;
            det.x2 = x2;
            det.y2 = y2;

            for (int j = 0; j < kFaceNumLandmarks; j++) {
                det.landmarks[j][0] = (lm[j * 2]     * stride + anchor_cx) * scale;
                det.landmarks[j][1] = (lm[j * 2 + 1] * stride + anchor_cy) * scale;
            }

            // clamp 到原图范围 (trt_face.cpp:678-682)
            det.x1 = std::max(0.0f, std::min(det.x1, static_cast<float>(orig_w)));
            det.y1 = std::max(0.0f, std::min(det.y1, static_cast<float>(orig_h)));
            det.x2 = std::max(0.0f, std::min(det.x2, static_cast<float>(orig_w)));
            det.y2 = std::max(0.0f, std::min(det.y2, static_cast<float>(orig_h)));

            dets.push_back(det);
        }
    }

    // Greedy NMS (trt_face.cpp:688-689)
    nms(dets, nms_thresh);

    // 拷贝结果 (trt_face.cpp:693-695)
    int count = static_cast<int>(std::min(static_cast<size_t>(max_dets), dets.size()));
    for (int i = 0; i < count; i++)
        out_dets[i] = dets[i];

    return count;
}

}  // namespace face_infer
