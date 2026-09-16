/// @file FaceProcessor.cpp
/// @brief FaceProcessor 编排类实现

#include "FaceProcessor.hpp"
#include "mem_management/dma_sync_guard.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace face_infer {

FaceProcessor::~FaceProcessor() {
    Destroy();
}

bool FaceProcessor::Init(const FaceProcessorConfig& config) {
    if (initialized_) {
        fprintf(stderr, "[FaceProcessor] Already initialized\n");
        return false;
    }
    config_ = config;

    // ① OpenCL 加载器 (单例)
    ocl_ = OpenClLoader::Get();
    if (!ocl_) {
        fprintf(stderr, "[FaceProcessor] OpenClLoader::Get() failed\n");
        return false;
    }

    // ② GPU 预处理 (OpenCL context + kernel 编译 + DmaBuffer 输出缓冲区)
    if (!gpu_.Init(config_.opencl_kernel_path, ocl_)) {
        fprintf(stderr, "[FaceProcessor] FacePreprocess::Init failed\n");
        return false;
    }
    fprintf(stderr, "[FaceProcessor] FacePreprocess initialized\n");

    // ③ SCRFD QNN (dlopen + contextCreateFromBinary + graphRetrieve)
    fprintf(stderr, "[FaceProcessor] SCRFD HTP backend-device-id=%d\n",
            config_.backend_device_id);
    if (!scrfd_qnn_.Init(config_.qnn_backend, config_.qnn_system,
                          config_.scrfd_model, config_.backend_device_id)) {
        fprintf(stderr, "[FaceProcessor] SCRFD QnnInferencer::Init failed\n");
        gpu_.Destroy();
        return false;
    }
    fprintf(stderr, "[FaceProcessor] SCRFD QNN initialized (%u output tensors)\n",
            scrfd_qnn_.GetNumOutputTensors());

    // ④ ArcFace QNN (可选)
    arcface_enabled_ = !config_.arcface_model.empty();
    if (arcface_enabled_) {
        if (!arcface_.Init(config_.qnn_backend, config_.qnn_system,
                           config_.arcface_model)) {
            fprintf(stderr, "[FaceProcessor] ArcFace Init failed, "
                            "falling back to detection-only\n");
            arcface_enabled_ = false;
        } else {
            fprintf(stderr, "[FaceProcessor] ArcFace initialized\n");
        }
    } else {
        fprintf(stderr, "[FaceProcessor] ArcFace disabled (no model path)\n");
    }

    scrfd_cpu_copy_.resize(FacePreprocess::kScrfdOutputFloats);
    if (arcface_enabled_)
        arcface_cpu_copy_.resize(FacePreprocess::kArcfaceOutputFloats);

    initialized_ = true;
    fprintf(stderr, "[FaceProcessor] Init complete (SCRFD + %s)\n",
            arcface_enabled_ ? "ArcFace" : "detection-only");
    return true;
}

std::vector<FaceResult> FaceProcessor::ProcessFrame(
        cl_mem nv12_cl, const FramePlaneInfo& plane) {

    std::vector<FaceResult> results;
    if (!initialized_) {
        fprintf(stderr, "[FaceProcessor] Not initialized\n");
        return results;
    }

    // Phase A: SCRFD 全帧检测
    std::vector<FaceDetection> dets = DetectFaces(nv12_cl, plane);
    if (dets.empty()) return results;

    results.resize(dets.size());

    for (size_t i = 0; i < dets.size(); i++) {
        results[i].det = dets[i];

        // Phase B: ArcFace 嵌入提取 (如果启用)
        if (arcface_enabled_) {
            if (!ExtractEmbedding(nv12_cl, plane, dets[i],
                                  results[i].embedding)) {
                memset(results[i].embedding, 0,
                       sizeof(float) * kFaceEmbeddingDim);
            }
        } else {
            memset(results[i].embedding, 0,
                   sizeof(float) * kFaceEmbeddingDim);
        }
    }

    return results;
}

// ---- Phase A: SCRFD 全帧检测 ----

std::vector<FaceDetection> FaceProcessor::DetectFaces(
        cl_mem nv12_cl, const FramePlaneInfo& plane) {

    std::vector<FaceDetection> dets;

    // ① GPU 预处理: NV12 → NCHW 640×640 letterbox
    float scale = 0;
    int new_w = 0, new_h = 0;
    const float* scrfd_input = gpu_.RunScrfd(nv12_cl, plane,
                                              &scale, &new_w, &new_h);
    if (!scrfd_input) {
        fprintf(stderr, "[FaceProcessor] RunScrfd failed\n");
        return dets;
    }

    // CPU-consume GPU output under DMA sync, then drop the lock before HTP.
    const size_t input_bytes =
        FacePreprocess::kScrfdOutputFloats * sizeof(float);
    {
        if (stage_cb_) stage_cb_(face_async::FaceStage::DMA_SYNC, true);
        DmaSyncGuard scrfd_cpu(gpu_.ScrfdOutFd());
        std::memcpy(scrfd_cpu_copy_.data(), scrfd_input, input_bytes);
        if (stage_cb_) stage_cb_(face_async::FaceStage::DMA_SYNC, false);
    }

    // ② QNN SCRFD 推理
    if (!scrfd_qnn_.Execute(scrfd_cpu_copy_.data(), input_bytes)) {
        fprintf(stderr, "[FaceProcessor] SCRFD Execute failed\n");
        return dets;
    }

    // ③ 收集 9 个输出 tensor 指针
    uint32_t n_out = scrfd_qnn_.GetNumOutputTensors();
    if (n_out < 9) {
        fprintf(stderr, "[FaceProcessor] SCRFD expected 9 tensors, got %u\n",
                n_out);
        return dets;
    }

    const float* output_tensors[9];
    for (int i = 0; i < 9; i++) {
        output_tensors[i] = scrfd_qnn_.GetOutputData(
                static_cast<uint32_t>(i));
        if (!output_tensors[i]) {
            fprintf(stderr, "[FaceProcessor] SCRFD tensor[%d] is null\n", i);
            return dets;
        }
    }

    // ④ 解码 + NMS
    int max_dets = std::min(config_.max_faces, kFaceMaxDetections);
    std::vector<FaceDetection> det_buf(max_dets);
    if (stage_cb_) stage_cb_(face_async::FaceStage::SCRFD_DECODE, true);
    int n_det = ScrfdDecode(output_tensors, scale,
                            static_cast<int>(plane.width),
                            static_cast<int>(plane.height),
                            config_.conf_threshold,
                            config_.nms_threshold,
                            det_buf.data(), max_dets);
    if (stage_cb_) stage_cb_(face_async::FaceStage::SCRFD_DECODE, false);

    dets.assign(det_buf.begin(), det_buf.begin() + n_det);
    return dets;
}

// ---- Phase B: ArcFace 单人脸嵌入提取 ----

bool FaceProcessor::ExtractEmbedding(
        cl_mem nv12_cl, const FramePlaneInfo& plane,
        const FaceDetection& det,
        float out_embedding[kFaceEmbeddingDim]) {

    // ⑤ Umeyama 逆仿射
    float inv[6];
    ComputeInverseAffine(det.landmarks, inv);

    // ⑥ GPU 仿射裁剪: NV12 → NCHW 112×112
    const float* arcface_input = gpu_.RunArcface(nv12_cl, plane, inv);
    if (!arcface_input) {
        fprintf(stderr, "[FaceProcessor] RunArcface failed\n");
        return false;
    }

    const size_t input_bytes =
        FacePreprocess::kArcfaceOutputFloats * sizeof(float);
    {
        if (stage_cb_) stage_cb_(face_async::FaceStage::DMA_SYNC, true);
        DmaSyncGuard arcface_cpu(gpu_.ArcfaceOutFd());
        std::memcpy(arcface_cpu_copy_.data(), arcface_input, input_bytes);
        if (stage_cb_) stage_cb_(face_async::FaceStage::DMA_SYNC, false);
    }

    // ⑦ QNN ArcFace 推理 + L2 归一化
    if (!arcface_.Infer(arcface_cpu_copy_.data(), out_embedding)) {
        fprintf(stderr, "[FaceProcessor] ArcFace Infer failed\n");
        return false;
    }

    return true;
}

float FaceProcessor::Similarity(const float* emb1, const float* emb2) {
    return arcface_infer::ArcFaceInfer::Similarity(emb1, emb2);
}

cl_context FaceProcessor::GetContext() const {
    return gpu_.GetContext();
}

void FaceProcessor::Destroy() {
    if (!initialized_) return;

    arcface_.Destroy();
    scrfd_qnn_.Destroy();
    gpu_.Destroy();
    scrfd_cpu_copy_.clear();
    arcface_cpu_copy_.clear();

    initialized_ = false;
    arcface_enabled_ = false;
    fprintf(stderr, "[FaceProcessor] Destroyed\n");
}

}  // namespace face_infer
