/// @file frame_layout.hpp
/// @brief Pure single-FD NV12 frame layout validation.
///
/// This round supports and validates ONLY the board-proven layout:
/// one GstMemory, FD/DMA-BUF backed, Y and UV planes both inside the same FD.
/// Multi-memory layouts are rejected (producer cancels the copy and the main
/// video frame still passes through; results stay UNAVAILABLE).

#pragma once

#include <cstdint>
#include <string>

namespace face_async {

struct FrameLayout {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t y_offset = 0;
  std::uint32_t y_stride = 0;
  std::uint64_t uv_offset = 0;
  std::uint32_t uv_stride = 0;
  std::uint64_t buffer_size = 0;  ///< gst_buffer_get_size
};

/// Validate a single-FD NV12 frame for the copy:
///  1. SOURCE planes must be readable inside the decoder buffer — source
///     padding (e.g. uv_offset advanced to an aligned row) is ALLOWED;
///     only the actual row ranges are checked.
///  2. The COMPACT TARGET layout (actual strides, no source padding
///     replicated) must fit inside the slot capacity — the copy writes
///     Y@0 and UV@(y_stride*height) into the face-owned slot.
/// err_out (optional) receives a human-readable reason on failure.
inline bool validate_frame_layout(const FrameLayout& l,
                                  std::uint64_t slot_capacity,
                                  std::string* err_out = nullptr) {
  auto fail = [&](const char* why) {
    if (err_out) *err_out = why;
    return false;
  };
  if (l.width == 0 || l.height == 0) return fail("zero dims");
  if (l.height < 2) return fail("degenerate height");  // NV12 needs >=2 rows
  if (l.y_stride < l.width) return fail("y_stride < width");
  if (l.uv_stride < l.width) return fail("uv_stride < width");
  if (l.buffer_size == 0) return fail("empty buffer");

  const std::uint64_t uv_rows = l.height / 2;
  /* 1. source rows readable (padding between/around planes allowed) */
  const std::uint64_t src_y_end =
      l.y_offset + static_cast<std::uint64_t>(l.y_stride) * l.height;
  const std::uint64_t src_uv_end =
      l.uv_offset + static_cast<std::uint64_t>(l.uv_stride) * uv_rows;
  if (src_y_end > l.buffer_size) return fail("Y plane beyond buffer");
  if (src_uv_end > l.buffer_size) return fail("UV plane beyond buffer");

  /* 2. compact target layout fits the face-owned slot */
  const std::uint64_t compact =
      static_cast<std::uint64_t>(l.y_stride) * l.height +
      static_cast<std::uint64_t>(l.uv_stride) * uv_rows;
  if (compact > slot_capacity)
    return fail("compact layout beyond slot capacity");
  return true;
}

}  // namespace face_async
