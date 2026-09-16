/// @file dma_buffer.hpp
/// @brief DmaBuffer 工具类: DMA Heap 分配 + mmap + 可选 OpenCL 导入
///
/// 用法:
///   DmaBuffer buf;
///   buf.Init(size);                              // DMA_HEAP_IOCTL_ALLOC + mmap
///   buf.BindOpenCl(ctx, ocl);                    // 可选: 导入为 cl_mem
///   float* p = static_cast<float*>(buf.data());  // CPU 读写
///   cl_mem m = buf.cl_mem_handle();              // GPU 读写
///   buf.Destroy();                               // 释放全部资源

#pragma once

#include "opencl_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <CL/cl.h>

namespace face_infer {

class DmaBuffer {
public:
    DmaBuffer() = default;
    ~DmaBuffer();

    /// 从 /dev/dma_heap/qcom,system 分配 DMABuf 并 mmap
    /// @param size 字节数
    /// @return true 成功
    bool Init(size_t size);

    /// 将 DMABuf 导入到 OpenCL (可选, 仅 GPU 需要访问的 buffer 调用)
    /// 参考: overlay_lib.cc:354-387 MapBuffer
    /// @param ctx  OpenCL context
    /// @param ocl  OpenClLoader 实例
    /// @return true 成功
    bool BindOpenCl(cl_context ctx, std::shared_ptr<OpenClLoader> ocl);

    /// 释放全部资源: clReleaseMemObject + munmap + close(fd)
    void Destroy();

    // --- 访问器 ---
    int fd() const { return fd_; }
    void* data() const { return vaddr_; }
    size_t size() const { return size_; }
    cl_mem cl_mem_handle() const { return cl_buf_; }

    // 禁止拷贝
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;

    // 允许移动
    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;

private:
    int fd_ = -1; // DMA-Heap 分配返回的文件描述符
    void* vaddr_ = nullptr; // mmap返回的用户空间虚拟地址
    size_t size_ = 0; // 分配字节数
    cl_mem cl_buf_ = nullptr; // OpenCL缓冲对象
    std::shared_ptr<OpenClLoader> ocl_; // 持有 OpenCL 加载器引用，确保Destroy()时仍能调用clReleaseMemObject
};

}  // namespace face_infer
