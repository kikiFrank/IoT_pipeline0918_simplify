/// @file QnnInferencer.hpp
/// @brief 纯 C++ QNN SDK 推理封装 (无 GStreamer 依赖)
///
/// 从 scrfd_infer/QnnInferencer.hpp 复制, namespace 改为 arcface_infer 以解耦
///
/// 仅支持 cached .bin 模型 (contextCreateFromBinary)
/// 支持任意数量输出 tensor (无 GST_ML_MAX_TENSORS=8 限制)
/// 自动反量化: UFIXED_POINT_8/16 → float32
///
/// 参考: gst-plugin-faceqnn/ml-faceqnn-engine.cc

#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "../async/stage_reporter.hpp"

namespace arcface_infer {

class QnnInferencer {
public:
    QnnInferencer() = default;
    ~QnnInferencer();
    QnnInferencer(const QnnInferencer&) = delete;
    QnnInferencer& operator=(const QnnInferencer&) = delete;

    /// 加载 QNN 后端 + 系统库 + 模型, 分配所有 tensor 缓冲区
    bool Init(const std::string& backend_path,   // /usr/lib/libQnnHtp.so
              const std::string& system_path,     // /usr/lib/libQnnSystem.so
              const std::string& model_path);     // .bin cached model

    /// 执行推理: memcpy 输入 → graphExecute → 反量化输出
    bool Execute(const float* input_data, size_t input_bytes);

    /// 获取第 tensor_idx 个输出的 float32 数据指针
    const float* GetOutputData(uint32_t tensor_idx) const;
    /// 获取第 tensor_idx 个输出的元素数 (floats)
    uint32_t GetOutputTensorSize(uint32_t tensor_idx) const;
    /// 输出 tensor 总数
    uint32_t GetNumOutputTensors() const;
    /// 输出 tensor 名称
    std::string GetOutputTensorName(uint32_t tensor_idx) const;

    /// Diagnostic stage hooks (QNN_COPY / GRAPH_EXECUTE / DEQUANT); no-op when empty.
    void SetStageCallback(std::function<void(face_async::FaceStage, bool)> cb) {
        stage_cb_ = std::move(cb);
    }

    void Destroy();

private:
    std::function<void(face_async::FaceStage, bool)> stage_cb_;
    struct Impl;
    Impl* impl_ = nullptr;
    bool initialized_ = false;
};

}  // namespace arcface_infer
