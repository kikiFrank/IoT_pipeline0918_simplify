#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

#include <gst/gst.h>

#include "aibox/media/SnapshotEncoder.hpp"

namespace {

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

bool LooksLikeJpeg(const std::vector<uint8_t>& jpeg) {
  if (jpeg.size() < 4) return false;
  return jpeg[0] == 0xFF && jpeg[1] == 0xD8 &&
         jpeg[jpeg.size() - 2] == 0xFF && jpeg[jpeg.size() - 1] == 0xD9;
}

std::vector<uint8_t> MakePackedNv12(uint32_t width, uint32_t height,
                                    uint8_t y_value) {
  const size_t y_size = static_cast<size_t>(width) * height;
  std::vector<uint8_t> nv12(y_size * 3 / 2, 0x80);
  std::fill(nv12.begin(), nv12.begin() + static_cast<std::ptrdiff_t>(y_size),
            y_value);
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      nv12[row * width + col] = static_cast<uint8_t>(
          (y_value + col + row) & 0xFF);
    }
  }
  return nv12;
}

bool TestEncodeKnownNv12() {
  aibox::media::GstSnapshotEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const uint32_t width = 64;
  const uint32_t height = 48;
  const std::vector<uint8_t> nv12 = MakePackedNv12(width, height, 80);
  std::vector<uint8_t> jpeg;
  CHECK_TRUE(encoder.EncodeNv12(nv12.data(), width, height, jpeg));
  CHECK_TRUE(!jpeg.empty());
  CHECK_TRUE(LooksLikeJpeg(jpeg));
  std::cout << "[encoder] 64x48 jpeg_bytes=" << jpeg.size() << std::endl;
  return true;
}

bool TestPersistentPipelineReuse() {
  aibox::media::GstSnapshotEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const std::vector<uint8_t> a = MakePackedNv12(64, 48, 40);
  const std::vector<uint8_t> b = MakePackedNv12(64, 48, 200);
  std::vector<uint8_t> jpeg_a;
  std::vector<uint8_t> jpeg_b;
  CHECK_TRUE(encoder.EncodeNv12(a.data(), 64, 48, jpeg_a));
  CHECK_TRUE(encoder.EncodeNv12(b.data(), 64, 48, jpeg_b));
  CHECK_TRUE(LooksLikeJpeg(jpeg_a));
  CHECK_TRUE(LooksLikeJpeg(jpeg_b));
  return true;
}

bool TestCapsChange() {
  aibox::media::GstSnapshotEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const std::vector<uint8_t> small = MakePackedNv12(64, 48, 90);
  const std::vector<uint8_t> large = MakePackedNv12(128, 96, 90);
  std::vector<uint8_t> jpeg;
  CHECK_TRUE(encoder.EncodeNv12(small.data(), 64, 48, jpeg));
  CHECK_TRUE(LooksLikeJpeg(jpeg));
  CHECK_TRUE(encoder.EncodeNv12(large.data(), 128, 96, jpeg));
  CHECK_TRUE(LooksLikeJpeg(jpeg));
  return true;
}

bool TestRejectsOddSize() {
  aibox::media::GstSnapshotEncoder encoder;
  CHECK_TRUE(encoder.IsReady());
  const std::vector<uint8_t> nv12(64 * 47, 0);
  std::vector<uint8_t> jpeg;
  CHECK_TRUE(!encoder.EncodeNv12(nv12.data(), 64, 47, jpeg));
  CHECK_TRUE(jpeg.empty());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!aibox::media::GstSnapshotEncoder::JpegencAvailable()) {
    std::cerr << "SKIP jpegenc not available\n";
    return 77;
  }

  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"EncodeKnownNv12", TestEncodeKnownNv12},
      {"PersistentPipelineReuse", TestPersistentPipelineReuse},
      {"CapsChange", TestCapsChange},
      {"RejectsOddSize", TestRejectsOddSize},
  };

  for (const auto& test : tests) {
    if (!test.fn()) {
      std::cerr << "FAILED " << test.name << '\n';
      return 1;
    }
    std::cout << "PASSED " << test.name << '\n';
  }
  return 0;
}
