/// @file frame_layout_tests.cpp
/// @brief Scenario 14: single-FD NV12 layout validation —
///        multi-memory/oversize/bad-stride/degenerate cases must be rejected.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "async/frame_layout.hpp"

namespace fa = face_async;

namespace {

fa::FrameLayout GoodLayout() {
  fa::FrameLayout l;
  l.width = 640;
  l.height = 360;
  l.y_offset = 0;
  l.y_stride = 640;
  l.uv_offset = 640 * 360;
  l.uv_stride = 640;
  l.buffer_size = 640 * 360 + 640 * 180;
  return l;
}

bool Check(const fa::FrameLayout& l, std::uint64_t cap) {
  std::string err;
  return fa::validate_frame_layout(l, cap, &err);
}

}  // namespace

TEST(FrameLayoutTest, ValidSingleFdLayoutAccepted) {
  const auto l = GoodLayout();
  EXPECT_TRUE(Check(l, l.buffer_size));
}

TEST(FrameLayoutTest, InvalidLayoutsRejected) {
  const auto good = GoodLayout();

  fa::FrameLayout bad = good;
  bad.width = 0;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // zero width

  bad = good;
  bad.height = 0;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // zero height

  bad = good;
  bad.height = 1;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // degenerate

  bad = good;
  bad.y_stride = 100;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // stride < width

  bad = good;
  bad.uv_stride = 100;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // uv stride < width

  bad = good;
  bad.uv_offset = good.buffer_size - 1;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // UV beyond buffer

  bad = good;
  bad.y_stride = 4096;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // Y beyond buffer (source range)

  bad = good;
  bad.y_offset = good.uv_offset;  // Y plane pushed past the buffer end
  EXPECT_FALSE(Check(bad, good.buffer_size));  // Y offset + plane beyond buffer

  bad = good;
  EXPECT_FALSE(Check(bad, good.buffer_size - 1));  // slot capacity too small

  bad = good;
  bad.buffer_size = 0;
  EXPECT_FALSE(Check(bad, good.buffer_size));  // empty buffer
}

TEST(FrameLayoutTest, StridedSingleFdLayoutAccepted) {
  // Real decoders often pad strides; same-FD with padding must pass.
  auto l = GoodLayout();
  l.y_stride = 1024;
  l.uv_offset = 1024 * 360;
  l.uv_stride = 1024;
  l.buffer_size = 1024 * 360 + 1024 * 180;
  EXPECT_TRUE(Check(l, l.buffer_size));
}

TEST(FrameLayoutTest, PaddedSourceUvOffsetAcceptedWhenCompactFits) {
  // Real decoders may advance uv_offset past the compact boundary
  // (row-aligned padding). Source rows are checked against buffer_size;
  // the COMPACT target layout is checked against slot capacity.
  auto l = GoodLayout();
  l.uv_offset = static_cast<std::uint64_t>(l.y_stride) * 384;  // padded rows
  l.buffer_size = l.uv_offset + static_cast<std::uint64_t>(l.uv_stride) * (l.height / 2);
  EXPECT_TRUE(Check(l, GoodLayout().buffer_size));  // compact 345600 <= cap
}

TEST(FrameLayoutTest, PaddedStrideBeyondSlotCapacityRejected) {
  // Stride padding beyond the slot's aligned headroom: compact layout
  // exceeds capacity even though the source buffer itself is consistent.
  auto l = GoodLayout();
  l.y_stride = 1024;
  l.uv_offset = 1024 * 360;
  l.uv_stride = 1024;
  l.buffer_size = 1024 * 360 + 1024 * 180;
  EXPECT_FALSE(Check(l, GoodLayout().buffer_size));  // compact > cap
}
