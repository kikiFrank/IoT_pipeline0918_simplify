/// @file dma_sync_guard.hpp
/// @brief RAII DMA 缓冲区同步 (参考 overlay 插件 tools.h:80-112)
///
/// 用法:
///   {
///       DmaSyncGuard guard(dma_fd);  // → DMA_BUF_SYNC_START
///       float* p = (float*)buf.data();
///       // ... CPU 读写 ...
///   }  // → DMA_BUF_SYNC_END

/**
 * 在 GPU 写入 DMA 缓冲区后、CPU 读取前，执行必要的缓存同步操作。RAII 模式自动配对 `SYNC_START` / `SYNC_END`。
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

namespace face_infer {

class DmaSyncGuard {
public:
    /// @param fd    DMA-BUF fd
    /// @param flags DMA_BUF_SYNC_READ / DMA_BUF_SYNC_WRITE / DMA_BUF_SYNC_RW
    ///              (cpu-access direction; default RW keeps legacy behavior)
    explicit DmaSyncGuard(int fd, std::uint64_t flags = DMA_BUF_SYNC_RW)
        : fd_(fd), flags_(flags) {
        struct dma_buf_sync sync{};
        sync.flags = flags_ | DMA_BUF_SYNC_START;
        if (ioctl(fd_, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
            fprintf(stderr, "[DmaSyncGuard] DMA_BUF_SYNC_START failed fd=%d\n", fd_);
        }
    }

    ~DmaSyncGuard() {
        struct dma_buf_sync sync{};
        sync.flags = flags_ | DMA_BUF_SYNC_END;
        if (ioctl(fd_, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
            fprintf(stderr, "[DmaSyncGuard] DMA_BUF_SYNC_END failed fd=%d\n", fd_);
        }
    }

    DmaSyncGuard(const DmaSyncGuard&) = delete;
    DmaSyncGuard& operator=(const DmaSyncGuard&) = delete;

private:
    int fd_;
    std::uint64_t flags_;
};

}  // namespace face_infer
