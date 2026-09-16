/// @file ArcFaceInfer.hpp
/// @brief ArcFace QNN 推理封装: QnnInferencer + L2 归一化 + 相似度计算
///
/// 输入: float32 NCHW 3×112×112 (FacePreprocess::RunArcface 输出)
/// 输出: float32 512-dim L2 归一化嵌入向量
///
/// 算法移植自 test/Neel_cpp/trt_face.cpp:1139-1187
///
/// 用法:
///   ArcFaceInfer arc;
///   arc.Init("/usr/lib/libQnnHtp.so", "/usr/lib/libQnnSystem.so",
///            "w600k_r50_qcs9075.bin");
///   arc.Infer(preproc_float_ptr, embedding_512_out);
///   float sim = ArcFaceInfer::Similarity(emb1, emb2);
///   arc.Destroy();

#pragma once
#include "QnnInferencer.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace arcface_infer {

class ArcFaceInfer {
public:
    static constexpr uint32_t kInputFloats  = 3 * 112 * 112;  // 37,632
    static constexpr uint32_t kEmbeddingDim = 512;

    ArcFaceInfer() = default;
    ~ArcFaceInfer();
    ArcFaceInfer(const ArcFaceInfer&) = delete;
    ArcFaceInfer& operator=(const ArcFaceInfer&) = delete;

    /// 加载 ArcFace .bin 模型, 验证输出 tensor 为 1 个 512 维
    bool Init(const std::string& backend_path,
              const std::string& system_path,
              const std::string& model_path);

    /// 推理 + L2 归一化
    /// @param input_data    float32 NCHW 3×112×112 (RunArcface 输出)
    /// @param out_embedding float32[512], L2 归一化后写入
    /// @return true on success
    bool Infer(const float* input_data, float out_embedding[kEmbeddingDim]);

    /// 余弦相似度 (两个 L2 归一化向量的点积, 范围 [-1, 1])
    static float Similarity(const float* emb1, const float* emb2);

    void Destroy();

    /// 诊断用: 访问内部 QnnInferencer
    const QnnInferencer& GetInferencer() const { return qnn_; }

    /// Forward diagnostic stage hooks to the internal QnnInferencer.
    void SetStageCallback(std::function<void(face_async::FaceStage, bool)> cb) {
        qnn_.SetStageCallback(std::move(cb));
    }

private:
    void L2Normalize(const float* raw, float* out) const;
    QnnInferencer qnn_;
    bool initialized_ = false;
};

}  // namespace arcface_infer
