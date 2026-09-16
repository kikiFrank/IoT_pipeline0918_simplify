/// @file FaceTypes.hpp
/// @brief 人脸检测/识别公共数据结构 + SCRFD 解码声明
///
/// 数据结构移植自 test/Neel_cpp/trt_face.h
/// SCRFD 常量匹配 QNN contextCreateFromBinary 返回的 stride-grouped tensor 顺序

#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace face_infer {

// ---- 全局常量 (对应 trt_face.h FACE_MAX_DETECTIONS / FACE_EMBEDDING_DIM / FACE_NUM_LANDMARKS) ----
constexpr int kFaceMaxDetections = 32;
constexpr int kFaceEmbeddingDim  = 512;
constexpr int kFaceNumLandmarks  = 5;

// ---- SCRFD 模型常量 ----
constexpr int kScrfdInputSize   = 640;
constexpr int kScrfdNumStrides  = 3;
constexpr int kScrfdStrides[3]  = {8, 16, 32};
constexpr int kScrfdFmc         = 2;   // anchors per grid cell

// ---- 数据结构 (对应 trt_face.h FaceDetection / FaceResult) ----

/// 单个人脸检测结果
struct FaceDetection {
    float x1, y1, x2, y2;                     // bbox (原图坐标)
    float score;
    float landmarks[kFaceNumLandmarks][2];     // 5 点 landmark
};

/// 人脸检测 + 嵌入向量
struct FaceResult {
    FaceDetection det;
    float embedding[kFaceEmbeddingDim];        // L2 归一化 512 维向量
};

// ---- SCRFD 输出 tensor 索引映射 ----
//
// QNN contextCreateFromBinary 返回 stride-grouped 顺序:
//   [0] output_0(12800,1)score_s8   [1] output_3(12800,4)bbox_s8   [2] output_6(12800,10)kps_s8
//   [3] output_1(3200,1)score_s16   [4] output_4(3200,4)bbox_s16   [5] output_7(3200,10)kps_s16
//   [6] output_2(800,1)score_s32    [7] output_5(800,4)bbox_s32    [8] output_8(800,10)kps_s32
struct ScrfdTensorMap {
    static constexpr int score_idx[3]    = {0, 3, 6};
    static constexpr int bbox_idx[3]     = {1, 4, 7};
    static constexpr int kps_idx[3]      = {2, 5, 8};
    static constexpr int anchor_count[3] = {12800, 3200, 800};
};

/// SCRFD 输出解码: 锚点解码 + letterbox 逆映射 + Greedy NMS
///
/// 算法移植自 test/Neel_cpp/trt_face.cpp:620-698 (解码) + 213-243 (NMS)
///
/// @param output_tensors  9 个 float* 指针数组 (顺序同 ScrfdTensorMap)
/// @param scale           letterbox 缩放因子 (FacePreprocess::RunScrfd 输出)
/// @param orig_w, orig_h  原图宽高 (用于 clamp)
/// @param conf_thresh     置信度阈值 (推荐 0.5)
/// @param nms_thresh      NMS IoU 阈值 (推荐 0.4)
/// @param out_dets        输出检测数组 (调用者分配)
/// @param max_dets        最大输出检测数
/// @return 实际检测数
int ScrfdDecode(const float* const* output_tensors,
                float scale, int orig_w, int orig_h,
                float conf_thresh, float nms_thresh,
                FaceDetection* out_dets, int max_dets);

}  // namespace face_infer
