/// @file FaceProcessor.hpp
/// @brief 人脸检测+识别编排类: SCRFD + ArcFace 全链路串联
///
/// 输入: NV12 帧 cl_mem + FramePlaneInfo
/// 输出: vector<FaceResult> (bbox + landmarks + 512-dim embedding)
///
/// 内部链路 (所有子模块均已板上验证通过):
///   NV12 → FacePreprocess::RunScrfd()      — GPU letterbox 1.19ms
///        → QnnInferencer::Execute(SCRFD)   — HTP 推理 10.36ms
///        → ScrfdDecode() + NMS             — CPU 解码
///        → 每个人脸:
///            ComputeInverseAffine()        — Umeyama 逆仿射
///            FacePreprocess::RunArcface()  — GPU 仿射裁剪 0.107ms
///            ArcFaceInfer::Infer()         — HTP 推理 6.47ms + L2 归一化
///        → vector<FaceResult>

#pragma once

#include "async/stage_reporter.hpp"
#include "face_preprocess/FacePreprocess.hpp"
#include "face_preprocess/inverse_affine.hpp"
#include "scrfd_infer/FaceTypes.hpp"
#include "scrfd_infer/QnnInferencer.hpp"
#include "arcface_infer/ArcFaceInfer.hpp"
#include "mem_management/opencl_loader.hpp"
#include "mem_management/mem_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace face_infer {

/// FaceProcessor 初始化配置
struct FaceProcessorConfig {
    std::string qnn_backend   = "/usr/lib/libQnnHtp.so";
    std::string qnn_system    = "/usr/lib/libQnnSystem.so";
    std::string scrfd_model;                 // 必填: SCRFD .bin
    std::string arcface_model;               // 可选: 空 = 仅检测
    std::string opencl_kernel_path;          // 必填: .cl kernel 目录
    float conf_threshold = 0.5f;
    float nms_threshold  = 0.4f;
    int   max_faces      = kFaceMaxDetections;
    int   backend_device_id = 0;             // HTP core: 0 or 1 on dual-CDSP boards
};

class FaceProcessor {
public:
    FaceProcessor() = default;
    ~FaceProcessor();
    FaceProcessor(const FaceProcessor&) = delete;
    FaceProcessor& operator=(const FaceProcessor&) = delete;

    /// 初始化: 加载 SCRFD + ArcFace 模型, 初始化 OpenCL
    bool Init(const FaceProcessorConfig& config);

    /// 处理单帧: 已导入 OpenCL 的 NV12 cl_mem → 人脸检测 + 识别
    std::vector<FaceResult> ProcessFrame(cl_mem nv12_cl,
                                         const FramePlaneInfo& plane);

    /// 余弦相似度 (两个 L2 归一化嵌入向量)
    static float Similarity(const float* emb1, const float* emb2);

    void Destroy();

    /// 获取 OpenCL context (供外部 DmaBuffer::BindOpenCl 使用)
    cl_context GetContext() const;

    /// 是否启用 ArcFace 识别
    bool HasArcFace() const { return arcface_enabled_; }

    /// Diagnostic stage hooks (DMA_SYNC / SCRFD_DECODE here; CL_* forwarded to
    /// FacePreprocess, QNN_* forwarded to the QNN inferencers). No-op when empty.
    void SetStageCallback(std::function<void(face_async::FaceStage, bool)> cb) {
        stage_cb_ = std::move(cb);
        gpu_.SetStageCallback(stage_cb_);
        scrfd_qnn_.SetStageCallback(stage_cb_);
        arcface_.SetStageCallback(stage_cb_);
    }

private:
    /// Phase A: SCRFD 全帧检测
    std::vector<FaceDetection> DetectFaces(cl_mem nv12_cl,
                                            const FramePlaneInfo& plane);

    /// Phase B: 单人脸 ArcFace 嵌入提取
    bool ExtractEmbedding(cl_mem nv12_cl,
                          const FramePlaneInfo& plane,
                          const FaceDetection& det,
                          float out_embedding[kFaceEmbeddingDim]);

    std::shared_ptr<OpenClLoader>   ocl_;
    FacePreprocess                  gpu_;
    QnnInferencer                   scrfd_qnn_;
    arcface_infer::ArcFaceInfer     arcface_;

    FaceProcessorConfig config_;
    bool arcface_enabled_ = false;
    bool initialized_     = false;
    std::function<void(face_async::FaceStage, bool)> stage_cb_;

    // CPU snapshot of GPU dma-bufs. DmaSyncGuard must not span QNN
    // graphExecute: DMA_BUF_SYNC_RW held across HTP + Venus encode
    // deadlocks the whole pipeline after ~1-2 min of WebRTC.
    std::vector<float> scrfd_cpu_copy_;
    std::vector<float> arcface_cpu_copy_;

};

}  // namespace face_infer
