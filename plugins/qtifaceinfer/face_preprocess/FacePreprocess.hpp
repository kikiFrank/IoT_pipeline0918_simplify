/// @file FacePreprocess.hpp
/// @brief OpenCL GPU 预处理器: SCRFD letterbox + ArcFace 仿射裁剪
///
/// 输出缓冲区使用 DmaBuffer 实现零拷贝 GPU→CPU (test_dma_buffer #4 已验证)
///
/// 用法:
///   FacePreprocess gpu;
///   gpu.Init("/path/to/kernels", OpenClLoader::Get());
///   const float* scrfd = gpu.RunScrfd(nv12_cl, plane, &scale, &nw, &nh);
///   const float* arc   = gpu.ArcfacePreprocess(nv12_cl, plane, inv_affine);
///   gpu.Destroy();

#pragma once

#include "../async/stage_reporter.hpp"
#include "../mem_management/dma_buffer.hpp"
#include "../mem_management/opencl_loader.hpp"
#include "../mem_management/mem_types.hpp"

#include <CL/cl.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace face_infer {

class FacePreprocess {
public:
    FacePreprocess() = default;
    ~FacePreprocess();

    FacePreprocess(const FacePreprocess&) = delete;
    FacePreprocess& operator=(const FacePreprocess&) = delete;

    /// 初始化 OpenCL context, 编译内核, 分配固定输出缓冲区
    /// @param kernel_path 包含 scrfd_preprocess.cl 和 arcface_preprocess.cl 的目录
    /// @param ocl 共享的 OpenCL API 加载器 (OpenClLoader::Get() 单例)
    bool Init(const std::string& kernel_path, std::shared_ptr<OpenClLoader> ocl);

    /// 释放所有 OpenCL 资源
    void Destroy();

    /// SCRFD 预处理: NV12 DMABuf → float32 NCHW 640×640 letterbox
    /// @param nv12_buf  输入帧的 cl_mem (覆盖整个 NV12 帧)
    /// @param plane     帧平面信息
    /// @param out_scale    输出 letterbox 缩放因子 (用于 SCRFD 解码)
    /// @param out_new_w    输出 letterbox 有效宽
    /// @param out_new_h    输出 letterbox 有效高
    /// @return 成功时返回指向内部 DmaBuffer 的 float* (零拷贝), 失败返回 nullptr
    ///         指针在下次调用 RunScrfd 前有效
    const float* RunScrfd(cl_mem nv12_buf, const FramePlaneInfo& plane,
                          float* out_scale, int* out_new_w, int* out_new_h);

    /// ArcFace 预处理: NV12 DMABuf → float32 NCHW 112×112 仿射裁剪
    /// @param nv12_buf  同一帧的 cl_mem
    /// @param plane     同一帧的平面信息
    /// @param inv_affine 逆仿射矩阵 [m00, m01, m02, m10, m11, m12]
    /// @return 成功时返回指向内部 DmaBuffer 的 float* (零拷贝), 失败返回 nullptr
    ///         指针在下次调用 RunArcface 前有效
    const float* RunArcface(cl_mem nv12_buf, const FramePlaneInfo& plane,
                            const float inv_affine[6]);

    /// 获取 OpenCL context (供外部 DmaBuffer::BindOpenCl 使用)
    cl_context GetContext() const { return ctx_; }

    /// DMABuf fd of the SCRFD / ArcFace output buffers (for DmaSyncGuard
    /// around the CPU consume of RunScrfd / RunArcface results).
    int ScrfdOutFd() const { return scrfd_out_.fd(); }
    int ArcfaceOutFd() const { return arcface_out_.fd(); }

    static constexpr int kScrfdSize = 640;
    static constexpr int kArcfaceSize = 112;
    static constexpr int kScrfdOutputFloats = 3 * kScrfdSize * kScrfdSize;      // 1,228,800
    static constexpr int kArcfaceOutputFloats = 3 * kArcfaceSize * kArcfaceSize; // 37,632

    /// Diagnostic stage hooks (CL_ENQUEUE / CL_FINISH); no-op when empty.
    void SetStageCallback(
        std::function<void(face_async::FaceStage, bool)> cb) {
        stage_cb_ = std::move(cb);
    }

private:
    bool BuildProgram(const std::string& cl_path, cl_program* prog,
                      cl_kernel* kernel, const char* kernel_name);
    bool SetArg(cl_kernel k, cl_uint idx, size_t size, const void* val);

    cl_context ctx_ = nullptr;
    cl_command_queue queue_ = nullptr;
    cl_device_id device_ = nullptr;

    cl_program scrfd_prog_ = nullptr;
    cl_kernel scrfd_kernel_ = nullptr;
    cl_program arcface_prog_ = nullptr;
    cl_kernel arcface_kernel_ = nullptr;

    // 固定输出缓冲区 (DmaBuffer: Init 时一次分配, 帧间覆写复用)
    // GPU kernel 写入 cl_mem_handle() → CPU 通过 data() 零拷贝读取
    DmaBuffer scrfd_out_;    // 3*640*640*sizeof(float) = 4.7 MB
    DmaBuffer arcface_out_;  // 3*112*112*sizeof(float) = 144 KB

    std::shared_ptr<OpenClLoader> ocl_;
    std::function<void(face_async::FaceStage, bool)> stage_cb_;
    bool initialized_ = false;
};

}  // namespace face_infer
