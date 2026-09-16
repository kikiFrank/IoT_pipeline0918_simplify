/// @file ArcFaceInfer.cpp
/// @brief ArcFace QNN 推理实现: Init + Infer (L2 归一化) + Similarity
///
/// L2 归一化移植自 trt_face.cpp:1033-1038
/// 余弦相似度移植自 trt_face.cpp:1182-1187

#include "ArcFaceInfer.hpp"
#include <cstdio>
#include <cmath>

#define LOG_TAG "[ArcFaceInfer] "

namespace arcface_infer {

ArcFaceInfer::~ArcFaceInfer() {
    Destroy();
}

bool ArcFaceInfer::Init(const std::string& backend_path,
                         const std::string& system_path,
                         const std::string& model_path) {
    if (initialized_) {
        fprintf(stderr, LOG_TAG "Already initialized\n");
        return false;
    }

    if (!qnn_.Init(backend_path, system_path, model_path)) {
        fprintf(stderr, LOG_TAG "QnnInferencer Init failed\n");
        return false;
    }

    // 验证输出 tensor: 必须恰好 1 个, 大小 512
    uint32_t n_outputs = qnn_.GetNumOutputTensors();
    if (n_outputs != 1) {
        fprintf(stderr, LOG_TAG "Expected 1 output tensor, got %u\n", n_outputs);
        Destroy();
        return false;
    }

    uint32_t out_size = qnn_.GetOutputTensorSize(0);
    if (out_size != kEmbeddingDim) {
        fprintf(stderr, LOG_TAG "Expected output size %u, got %u\n",
                kEmbeddingDim, out_size);
        Destroy();
        return false;
    }

    std::string name = qnn_.GetOutputTensorName(0);
    fprintf(stderr, LOG_TAG "Output tensor: '%s' size=%u\n",
            name.c_str(), out_size);

    initialized_ = true;
    fprintf(stderr, LOG_TAG "Init complete\n");
    return true;
}

bool ArcFaceInfer::Infer(const float* input_data,
                          float out_embedding[kEmbeddingDim]) {
    if (!initialized_) return false;

    if (!qnn_.Execute(input_data, kInputFloats * sizeof(float))) {
        fprintf(stderr, LOG_TAG "QNN Execute failed\n");
        return false;
    }

    const float* raw = qnn_.GetOutputData(0);
    if (!raw) {
        fprintf(stderr, LOG_TAG "GetOutputData(0) returned nullptr\n");
        return false;
    }

    L2Normalize(raw, out_embedding);
    return true;
}

void ArcFaceInfer::L2Normalize(const float* raw, float* out) const {
    // 移植自 trt_face.cpp:1033-1038 (trt_face_read_embedding)
    float norm = 0.0f;
    for (uint32_t i = 0; i < kEmbeddingDim; i++)
        norm += raw[i] * raw[i];
    norm = sqrtf(norm + 1e-10f);  // epsilon 防除零
    for (uint32_t i = 0; i < kEmbeddingDim; i++)
        out[i] = raw[i] / norm;
}

float ArcFaceInfer::Similarity(const float* emb1, const float* emb2) {
    // 移植自 trt_face.cpp:1182-1187 (trt_face_similarity)
    // 两个 L2 归一化向量的点积 = 余弦相似度, 范围 [-1, 1]
    float dot = 0.0f;
    for (uint32_t i = 0; i < kEmbeddingDim; i++)
        dot += emb1[i] * emb2[i];
    return dot;
}

void ArcFaceInfer::Destroy() {
    qnn_.Destroy();
    initialized_ = false;
}

}  // namespace arcface_infer
