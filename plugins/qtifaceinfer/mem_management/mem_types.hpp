/// @file mem_types.hpp
/// @brief 输入帧平面信息 (从 GstBuffer 提取)

#pragma once

#include <cstdint>

namespace face_infer {

/// 从 GstBuffer 提取的帧平面信息 (输入 NV12, 上游 v4l2h264dec 管理)
struct FramePlaneInfo {
    int fd = -1;              ///< DMABuf fd
    uint32_t frame_len = 0;   ///< 总字节长度 (Y + UV)
    uint32_t y_offset = 0;    ///< Y 平面偏移
    uint32_t uv_offset = 0;   ///< UV 平面偏移
    uint32_t y_stride = 0;    ///< Y 行步长
    uint32_t uv_stride = 0;   ///< UV 行步长
    uint32_t width = 0;       ///< 帧宽度
    uint32_t height = 0;      ///< 帧高度
};

}  // namespace face_infer
