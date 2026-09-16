/// @file dma_buffer.cpp
/// @brief DmaBuffer 实现: DMA Heap 分配 + mmap + 可选 OpenCL 导入
///
/// 参考:
///   overlay_lib.cc:354-387 MapBuffer   → BindOpenCl
///   overlay_lib.cc:390-402 UnMapBuffer → Destroy
///   gstqtiallocator.c                  → DMA_HEAP_IOCTL_ALLOC

#include "dma_buffer.hpp"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>
#include <CL/cl_ext_qcom.h>

namespace face_infer {

// ============================================================
// 析构
// ============================================================
DmaBuffer::~DmaBuffer() {
    Destroy();
}

// ============================================================
// Init: DMA Heap 分配 + mmap
// ============================================================
bool DmaBuffer::Init(size_t size) {
    if (fd_ >= 0) {
        fprintf(stderr, "[DmaBuffer] already initialized, call Destroy() first\n");
        return false;
    }

    /**
     * DMA分配 + 内存映射
     * 
     */
    // 打开Qualcomm DMA Heap设备；
    int heap_fd = open("/dev/dma_heap/qcom,system", O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0) {
        fprintf(stderr, "[DmaBuffer] open /dev/dma_heap/qcom,system failed: %s\n",
                strerror(errno));
        return false;
    }

    struct dma_heap_allocation_data alloc {};
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;

    // ioctl(DMA_HEAP_IOCTL_ALLOC) 分配指定字节数的 DMA 缓冲区，获得 fd
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        fprintf(stderr, "[DmaBuffer] DMA_HEAP_IOCTL_ALLOC %zu bytes failed: %s\n",
                size, strerror(errno));
        close(heap_fd);
        return false;
    }
    close(heap_fd); //Heap设备fd可以关闭，buffer fd独立

    fd_ = alloc.fd;
    // mmap(MAP_SHARED)映射到用户空间虚拟地址。
    vaddr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (vaddr_ == MAP_FAILED) {
        fprintf(stderr, "[DmaBuffer] mmap %zu bytes failed: %s\n", size, strerror(errno));
        close(fd_);
        fd_ = -1;
        vaddr_ = nullptr;
        return false;
    }

    size_ = size;
    fprintf(stderr, "[DmaBuffer] Init ok: fd=%d size=%zu vaddr=%p\n", fd_, size_, vaddr_);
    return true;
}

// ============================================================
// BindOpenCl: 零拷贝GPU导入
// 将 DMABuf 导入为 cl_mem
// 参考 overlay_lib.cc:354-387 MapBuffer
// ============================================================
bool DmaBuffer::BindOpenCl(cl_context ctx, std::shared_ptr<OpenClLoader> ocl) {
    if (fd_ < 0 || !vaddr_) {
        fprintf(stderr, "[DmaBuffer] BindOpenCl: buffer not initialized\n");
        return false;
    }
    if (cl_buf_) {
        fprintf(stderr, "[DmaBuffer] BindOpenCl: already bound\n");
        return false;
    }

    ocl_ = ocl;

    // cl_mem_ion_host_ptr: 名称遗留自 ION, 实际使用 CL_MEM_DMABUF_HOST_PTR_QCOM
    cl_mem_ion_host_ptr ionmem {};
    ionmem.ext_host_ptr.allocation_type   = CL_MEM_DMABUF_HOST_PTR_QCOM; // Qualcomm DMABuf 导入
    ionmem.ext_host_ptr.host_cache_policy = CL_MEM_HOST_IOCOHERENT_QCOM;// IO 一致性缓存
    ionmem.ion_hostptr  = vaddr_;
    ionmem.ion_filedesc = fd_;

    cl_int rc;
    cl_mem_flags flags = CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM;
    // 创建DMABuf
    cl_buf_ = ocl_->CreateBuffer(ctx, flags, size_, &ionmem, &rc);
    if (rc != CL_SUCCESS) {
        fprintf(stderr, "[DmaBuffer] clCreateBuffer failed: rc=%d\n", rc);
        cl_buf_ = nullptr;
        ocl_.reset();
        return false;
    }

    fprintf(stderr, "[DmaBuffer] BindOpenCl ok: cl_mem=%p\n", (void*)cl_buf_);
    return true;
}

// ============================================================
// Destroy: 释放全部资源
// 参考 overlay_lib.cc:390-402 UnMapBuffer
// ============================================================
void DmaBuffer::Destroy() {
    if (cl_buf_) {
        if (ocl_) {
            ocl_->ReleaseMemObject(cl_buf_);
        }
        cl_buf_ = nullptr;
    }
    if (vaddr_ && vaddr_ != MAP_FAILED) {
        munmap(vaddr_, size_);
        vaddr_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    size_ = 0;
    ocl_.reset();
}

// ============================================================
// 移动语义
// ============================================================
DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept //移动构造：转移所有资源所有权(fd_,vaddr_,size_,cl_buf_,ocl_)，源对象重置为默认值
    : fd_(other.fd_),
      vaddr_(other.vaddr_),
      size_(other.size_),
      cl_buf_(other.cl_buf_),
      ocl_(std::move(other.ocl_)) {
    other.fd_ = -1;
    other.vaddr_ = nullptr;
    other.size_ = 0;
    other.cl_buf_ = nullptr;
}

// 移动赋值: 先 Destroy()自身已有资源，再转移。支持自赋值检测
DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept {
    if (this != &other) {
        Destroy();
        fd_     = other.fd_;
        vaddr_  = other.vaddr_;
        size_   = other.size_;
        cl_buf_ = other.cl_buf_;
        ocl_    = std::move(other.ocl_);

        other.fd_ = -1;
        other.vaddr_ = nullptr;
        other.size_ = 0;
        other.cl_buf_ = nullptr;
    }
    return *this;
}

}  // namespace face_infer
