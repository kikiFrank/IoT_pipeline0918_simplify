#include <cstdint>
#include <iostream>
#include <vector>

#include <gst/gst.h>

#include "aibox/media/ThumbnailEncoder.hpp"

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

// Minimal JPEG SOF parser (no image library in this project): walk markers
// from offset 2; on SOF0/SOF1/SOF2 read precision, height, width.
bool ReadSofSize(const std::vector<uint8_t>& jpeg, uint32_t& width_out,
                 uint32_t& height_out) {
  size_t pos = 2;
  const size_t n = jpeg.size();
  while (pos + 4 <= n) {
    if (jpeg[pos] != 0xFF) return false;
    const uint8_t marker = jpeg[pos + 1];
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      pos += 2;  // standalone markers
      continue;
    }
    if (pos + 4 > n) return false;
    const uint16_t seg_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(jpeg[pos + 2]) << 8) | jpeg[pos + 3]);
    if (seg_len < 2 || pos + 2 + seg_len > n) return false;
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      if (seg_len < 7) return false;
      height_out = static_cast<uint32_t>(
          (static_cast<uint16_t>(jpeg[pos + 5]) << 8) | jpeg[pos + 6]);
      width_out = static_cast<uint32_t>(
          (static_cast<uint16_t>(jpeg[pos + 7]) << 8) | jpeg[pos + 8]);
      return true;
    }
    pos += 2 + seg_len;
  }
  return false;
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

bool IsThumbSize(const std::vector<uint8_t>& jpeg) {
  uint32_t w = 0, h = 0;
  if (!ReadSofSize(jpeg, w, h)) return false;
  return w == aibox::media::IThumbnailEncoder::kThumbWidth &&
         h == aibox::media::IThumbnailEncoder::kThumbHeight;
}

bool TestEncodeFullRes() {
  aibox::media::GstThumbnailEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const uint32_t width = 2048;
  const uint32_t height = 1152;
  const std::vector<uint8_t> nv12 = MakePackedNv12(width, height, 80);
  std::vector<uint8_t> thumb;
  CHECK_TRUE(encoder.EncodeNv12(nv12.data(), width, height, thumb));
  CHECK_TRUE(!thumb.empty());
  CHECK_TRUE(LooksLikeJpeg(thumb));
  CHECK_TRUE(IsThumbSize(thumb));
  std::cout << "[thumb] 2048x1152 -> jpeg_bytes=" << thumb.size() << std::endl;
  return true;
}

bool TestEncodeSmall() {
  aibox::media::GstThumbnailEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const std::vector<uint8_t> nv12 = MakePackedNv12(64, 48, 120);
  std::vector<uint8_t> thumb;
  CHECK_TRUE(encoder.EncodeNv12(nv12.data(), 64, 48, thumb));
  CHECK_TRUE(LooksLikeJpeg(thumb));
  CHECK_TRUE(IsThumbSize(thumb));
  return true;
}

bool TestCapsChange() {
  aibox::media::GstThumbnailEncoder encoder;
  CHECK_TRUE(encoder.IsReady());

  const std::vector<uint8_t> small = MakePackedNv12(64, 48, 90);
  const std::vector<uint8_t> large = MakePackedNv12(2048, 1152, 90);
  std::vector<uint8_t> thumb;
  CHECK_TRUE(encoder.EncodeNv12(small.data(), 64, 48, thumb));
  CHECK_TRUE(IsThumbSize(thumb));
  CHECK_TRUE(encoder.EncodeNv12(large.data(), 2048, 1152, thumb));
  CHECK_TRUE(IsThumbSize(thumb));
  CHECK_TRUE(encoder.EncodeNv12(small.data(), 64, 48, thumb));
  CHECK_TRUE(IsThumbSize(thumb));
  return true;
}

bool TestRejectsOddSize() {
  aibox::media::GstThumbnailEncoder encoder;
  CHECK_TRUE(encoder.IsReady());
  const std::vector<uint8_t> nv12(64 * 47, 0);
  std::vector<uint8_t> thumb;
  CHECK_TRUE(!encoder.EncodeNv12(nv12.data(), 64, 47, thumb));
  CHECK_TRUE(thumb.empty());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!aibox::media::GstThumbnailEncoder::Available()) {
    std::cerr << "SKIP jpegenc/videoscale/videoconvert not available\n";
    return 77;
  }

  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"EncodeFullRes", TestEncodeFullRes},
      {"EncodeSmall", TestEncodeSmall},
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
