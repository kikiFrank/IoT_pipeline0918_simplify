/// @file FacePreprocess.cpp
/// @brief OpenCL GPU 预处理实现: SCRFD letterbox + ArcFace 仿射裁剪
///
/// 参考:
///   overlay_lib.cc:139-193  OpenCL Init (platform/device/context/queue)
///   overlay_lib.cc:287-332  BuildProgram (.cl 编译)
///   face_preprocess.cu      CUDA 内核 → OpenCL 移植

#include "FacePreprocess.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// Qualcomm OpenCL context performance hint (参考 overlay_lib.cc:72-77)
#define CL_CONTEXT_PERF_HINT_QCOM  0x40C2
#define CL_PERF_HINT_HIGH_QCOM     0x40C3

namespace face_infer {

// ============================================================
// 析构
// ============================================================
FacePreprocess::~FacePreprocess() {
    Destroy();
}

// ============================================================
// Init
// ============================================================
bool FacePreprocess::Init(const std::string& kernel_path,
                           std::shared_ptr<OpenClLoader> ocl) {
    if (initialized_) {
        fprintf(stderr, "[FacePreprocess] already initialized\n");
        return false;
    }
    if (!ocl || !ocl->GetPlatformIDs) {
        fprintf(stderr, "[FacePreprocess] invalid OpenClLoader\n");
        return false;
    }
    ocl_ = ocl;

    // ① Platform
    cl_platform_id plat = nullptr;
    cl_uint num = 0;
    cl_int rc = ocl_->GetPlatformIDs(1, &plat, &num);
    if (rc != CL_SUCCESS || num == 0) {
        fprintf(stderr, "[FacePreprocess] clGetPlatformIDs failed: %d\n", rc);
        Destroy();
        return false;
    }

    // ② Device (GPU)
    rc = ocl_->GetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &device_, &num);
    if (rc != CL_SUCCESS || num == 0) {
        fprintf(stderr, "[FacePreprocess] clGetDeviceIDs(GPU) failed: %d\n", rc);
        Destroy();
        return false;
    }

    // ③ Context (参考 overlay_lib.cc:155)
    cl_context_properties props[] = {
        CL_CONTEXT_PLATFORM, (cl_context_properties)plat,
        CL_CONTEXT_PERF_HINT_QCOM, CL_PERF_HINT_HIGH_QCOM,
        0
    };
    ctx_ = ocl_->CreateContext(props, 1, &device_, nullptr, nullptr, &rc);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] clCreateContext failed: %d\n", rc);
        Destroy();
        return false;
    }

    // ④ Command Queue
    queue_ = ocl_->CreateCommandQueue(ctx_, device_, 0, &rc);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] CreateCommandQueue failed: %d\n", rc);
        Destroy();
        return false;
    }

    // ⑤ Build kernels
    std::string scrfd_cl = kernel_path + "/scrfd_preprocess.cl";
    std::string arcface_cl = kernel_path + "/arcface_preprocess.cl";

    if (!BuildProgram(scrfd_cl, &scrfd_prog_, &scrfd_kernel_, "scrfd_preprocess")) {
        Destroy();
        return false;
    }
    if (!BuildProgram(arcface_cl, &arcface_prog_, &arcface_kernel_, "arcface_preprocess")) {
        Destroy();
        return false;
    }

    // ⑥ 分配固定输出缓冲区 (DmaBuffer: DMA Heap + mmap + OpenCL 绑定)
    if (!scrfd_out_.Init(kScrfdOutputFloats * sizeof(float))) {
        fprintf(stderr, "[FacePreprocess] scrfd_out_ DmaBuffer Init failed\n");
        Destroy();
        return false;
    }
    if (!scrfd_out_.BindOpenCl(ctx_, ocl_)) {
        fprintf(stderr, "[FacePreprocess] scrfd_out_ BindOpenCl failed\n");
        Destroy();
        return false;
    }

    if (!arcface_out_.Init(kArcfaceOutputFloats * sizeof(float))) {
        fprintf(stderr, "[FacePreprocess] arcface_out_ DmaBuffer Init failed\n");
        Destroy();
        return false;
    }
    if (!arcface_out_.BindOpenCl(ctx_, ocl_)) {
        fprintf(stderr, "[FacePreprocess] arcface_out_ BindOpenCl failed\n");
        Destroy();
        return false;
    }

    initialized_ = true;
    fprintf(stderr, "[FacePreprocess] Init ok: scrfd_out=%p(%d) arcface_out=%p(%d)\n",
            scrfd_out_.data(), scrfd_out_.fd(),
            arcface_out_.data(), arcface_out_.fd());
    return true;
}

// ============================================================
// Destroy
// ============================================================
void FacePreprocess::Destroy() {
    scrfd_out_.Destroy();
    arcface_out_.Destroy();

    if (scrfd_kernel_)  { ocl_->ReleaseKernel(scrfd_kernel_);   scrfd_kernel_ = nullptr; }
    if (arcface_kernel_){ ocl_->ReleaseKernel(arcface_kernel_);  arcface_kernel_ = nullptr; }
    if (scrfd_prog_)    { ocl_->ReleaseProgram(scrfd_prog_);    scrfd_prog_ = nullptr; }
    if (arcface_prog_)  { ocl_->ReleaseProgram(arcface_prog_);  arcface_prog_ = nullptr; }
    if (queue_)         { ocl_->ReleaseCommandQueue(queue_);    queue_ = nullptr; }
    if (ctx_)           { ocl_->ReleaseContext(ctx_);           ctx_ = nullptr; }

    device_ = nullptr;
    ocl_.reset();
    initialized_ = false;
}

// ============================================================
// BuildProgram (参考 overlay_lib.cc:287-332)
// ============================================================
bool FacePreprocess::BuildProgram(const std::string& cl_path, cl_program* prog,
                                   cl_kernel* kernel, const char* kernel_name) {
    // 读取 .cl 文件
    std::ifstream ifs(cl_path);
    if (!ifs.is_open()) {
        fprintf(stderr, "[FacePreprocess] cannot open %s\n", cl_path.c_str());
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string source = oss.str();
    const char* src_ptr = source.c_str();
    size_t src_len = source.size();

    cl_int rc;
    *prog = ocl_->CreateProgramWithSource(ctx_, 1, &src_ptr, &src_len, &rc);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] clCreateProgramWithSource(%s) failed: %d\n",
                cl_path.c_str(), rc);
        return false;
    }

    rc = ocl_->BuildProgram(*prog, 1, &device_, "-cl-fast-relaxed-math", nullptr, nullptr);
    if (rc != CL_SUCCESS) {
        char log[4096] = {};
        ocl_->GetProgramBuildInfo(*prog, device_, CL_PROGRAM_BUILD_LOG,
                                  sizeof(log), log, nullptr);
        fprintf(stderr, "[FacePreprocess] clBuildProgram(%s) failed: %d\n%s\n",
                cl_path.c_str(), rc, log);
        ocl_->ReleaseProgram(*prog);
        *prog = nullptr;
        return false;
    }

    *kernel = ocl_->CreateKernel(*prog, kernel_name, &rc);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] clCreateKernel(%s) failed: %d\n", kernel_name, rc);
        ocl_->ReleaseProgram(*prog);
        *prog = nullptr;
        return false;
    }

    fprintf(stderr, "[FacePreprocess] built %s → kernel '%s'\n", cl_path.c_str(), kernel_name);
    return true;
}

// ============================================================
// SetArg 辅助
// ============================================================
bool FacePreprocess::SetArg(cl_kernel k, cl_uint idx, size_t size, const void* val) {
    cl_int rc = ocl_->SetKernelArg(k, idx, size, val);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] SetKernelArg(%u) failed: %d\n", idx, rc);
        return false;
    }
    return true;
}

// ============================================================
// RunScrfd: NV12 → NCHW 640×640 letterbox
// 移植自 face_preprocess.cu:230-261 (cuda_scrfd_preprocess)
// ============================================================
const float* FacePreprocess::RunScrfd(cl_mem nv12_buf, const FramePlaneInfo& plane,
                                       float* out_scale, int* out_new_w, int* out_new_h) {
    if (!initialized_) {
        fprintf(stderr, "[FacePreprocess] RunScrfd: not initialized\n");
        return nullptr;
    }

    // ① Letterbox 参数计算 (face_preprocess.cu:237-254)
    float im_ratio = (float)plane.height / (float)plane.width;
    int new_w, new_h;
    if (im_ratio > 1.0f) {
        new_h = kScrfdSize;
        new_w = (int)((float)kScrfdSize / im_ratio);
    } else {
        new_w = kScrfdSize;
        new_h = (int)((float)kScrfdSize * im_ratio);
    }
    new_w = std::min(new_w, kScrfdSize);
    new_h = std::min(new_h, kScrfdSize);
    float scale = (float)plane.width / (float)new_w;

    // ② 设置 12 个 kernel 参数
    cl_mem out_cl = scrfd_out_.cl_mem_handle();
    cl_uint arg = 0;
    uint32_t dst_size = kScrfdSize;
    uint32_t nw = (uint32_t)new_w;
    uint32_t nh = (uint32_t)new_h;

    bool ok = true;
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(cl_mem),   &nv12_buf);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(cl_mem),   &out_cl);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.y_offset);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.uv_offset);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.y_stride);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.uv_stride);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.width);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &plane.height);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &dst_size);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &nw);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(uint32_t), &nh);
    ok = ok && SetArg(scrfd_kernel_, arg++, sizeof(float),    &scale);
    if (!ok) return nullptr;

    // ③ Dispatch: 640×640, workgroup 16×16
    size_t global[2] = { (size_t)kScrfdSize, (size_t)kScrfdSize };
    size_t local[2]  = { 16, 16 };
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_ENQUEUE, true);
    cl_int rc = ocl_->EnqueueNDRangeKernel(queue_, scrfd_kernel_, 2,
                                            nullptr, global, local,
                                            0, nullptr, nullptr);
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_ENQUEUE, false);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] RunScrfd EnqueueNDRangeKernel failed: %d\n", rc);
        return nullptr;
    }

    // ④ 等待完成 + 零拷贝读取
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_FINISH, true);
    rc = ocl_->Finish(queue_);
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_FINISH, false);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] RunScrfd clFinish failed: %d\n", rc);
        return nullptr;
    }

    // CPU consume of scrfd_out_ is wrapped by DmaSyncGuard in FaceProcessor.

    // ⑤ 输出
    *out_scale = scale;
    *out_new_w = new_w;
    *out_new_h = new_h;
    return static_cast<const float*>(scrfd_out_.data());
}

// ============================================================
// RunArcface: NV12 → NCHW 112×112 仿射裁剪
// 移植自 face_preprocess.cu:263-276 (cuda_arcface_crop)
// ============================================================
const float* FacePreprocess::RunArcface(cl_mem nv12_buf, const FramePlaneInfo& plane,
                                         const float inv_affine[6]) {
    if (!initialized_) {
        fprintf(stderr, "[FacePreprocess] RunArcface: not initialized\n");
        return nullptr;
    }

    // ① 设置 15 个 kernel 参数
    cl_mem out_cl = arcface_out_.cl_mem_handle();
    cl_uint arg = 0;
    uint32_t dst_size = kArcfaceSize;

    bool ok = true;
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(cl_mem),   &nv12_buf);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(cl_mem),   &out_cl);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.y_offset);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.uv_offset);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.y_stride);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.uv_stride);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.width);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &plane.height);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(uint32_t), &dst_size);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[0]);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[1]);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[2]);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[3]);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[4]);
    ok = ok && SetArg(arcface_kernel_, arg++, sizeof(float),    &inv_affine[5]);
    if (!ok) return nullptr;

    // ② Dispatch: 112×112, workgroup 16×16
    size_t global[2] = { (size_t)kArcfaceSize, (size_t)kArcfaceSize };
    size_t local[2]  = { 16, 16 };
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_ENQUEUE, true);
    cl_int rc = ocl_->EnqueueNDRangeKernel(queue_, arcface_kernel_, 2,
                                            nullptr, global, local,
                                            0, nullptr, nullptr);
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_ENQUEUE, false);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] RunArcface EnqueueNDRangeKernel failed: %d\n", rc);
        return nullptr;
    }

    // ③ 等待完成 + 零拷贝读取
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_FINISH, true);
    rc = ocl_->Finish(queue_);
    if (stage_cb_) stage_cb_(face_async::FaceStage::CL_FINISH, false);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[FacePreprocess] RunArcface clFinish failed: %d\n", rc);
        return nullptr;
    }

    // CPU consume of arcface_out_ is wrapped by DmaSyncGuard in FaceProcessor.

    return static_cast<const float*>(arcface_out_.data());
}

}  // namespace face_infer
