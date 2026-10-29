#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include "aibox/siglip_worker/WorkerConfig.hpp"

#include <jpeglib.h>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

namespace aibox::siglip {
namespace {
struct JpegError {
  jpeg_error_mgr base;
  jmp_buf jump;
  unsigned char* pixels = nullptr;
};
void JpegFail(j_common_ptr ptr) {
  auto* err = reinterpret_cast<JpegError*>(ptr->err);
  longjmp(err->jump, 1);
}
struct Tap { int index; double weight; };
std::vector<std::vector<Tap>> BilinearTaps(int input, int output) {
  const double scale = static_cast<double>(input) / output;
  const double filter_scale = std::max(1.0, scale);
  std::vector<std::vector<Tap>> rows(output);
  for (int out = 0; out < output; ++out) {
    const double center = (out + 0.5) * scale;
    const int first = std::max(0, static_cast<int>(center - filter_scale + 0.5));
    const int last = std::min(input, static_cast<int>(center + filter_scale + 0.5));
    double total = 0;
    for (int pos = first; pos < last; ++pos) {
      const double weight = std::max(0.0, 1.0 - std::abs((pos - center + 0.5) / filter_scale));
      rows[out].push_back({pos, weight});
      total += weight;
    }
    if (total == 0) throw std::runtime_error("empty bilinear filter");
    for (auto& tap : rows[out]) tap.weight /= total;
  }
  return rows;
}
std::uint8_t RoundByte(double value) {
  return static_cast<std::uint8_t>(std::clamp(std::floor(value + 0.5), 0.0, 255.0));
}
}  // namespace

PreparedImage PrepareImage(const std::vector<std::uint8_t>& jpeg) {
  if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
    throw std::runtime_error("source is not JPEG");
  PreparedImage result;
  result.source_sha256 = Sha256(jpeg.data(), jpeg.size());
  auto decoder_state = std::make_unique<jpeg_decompress_struct>();
  auto& decoder = *decoder_state;
  // Heap state survives longjmp; no C++ object is constructed across a JPEG call.
  auto error = std::make_unique<JpegError>();
  decoder.err = jpeg_std_error(&error->base);
  error->base.error_exit = JpegFail;
  if (setjmp(error->jump)) {
    std::free(error->pixels);
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("JPEG decode failed");
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, jpeg.data(), jpeg.size());
  jpeg_read_header(&decoder, TRUE);
  if (decoder.image_width > 8192 || decoder.image_height > 8192 ||
      static_cast<std::uint64_t>(decoder.image_width) * decoder.image_height > 20000000) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("JPEG dimensions outside limit");
  }
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  const int width = static_cast<int>(decoder.output_width);
  const int height = static_cast<int>(decoder.output_height);
  if (width < 1 || height < 1 || width > 8192 || height > 8192 ||
      static_cast<std::int64_t>(width) * height > 20000000 || decoder.output_components != 3) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("JPEG dimensions or channels outside limit");
  }
  result.source_width = width;
  result.source_height = height;
  const auto rgb_size = static_cast<std::size_t>(width) * height * 3;
  error->pixels = static_cast<unsigned char*>(std::malloc(rgb_size));
  if (!error->pixels) {
    jpeg_destroy_decompress(&decoder);
    throw std::bad_alloc();
  }
  while (decoder.output_scanline < decoder.output_height) {
    JSAMPROW line = error->pixels + static_cast<std::size_t>(decoder.output_scanline) * width * 3;
    jpeg_read_scanlines(&decoder, &line, 1);
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  std::unique_ptr<unsigned char, decltype(&std::free)> pixels(error->pixels, &std::free);
  error->pixels = nullptr;
  const auto* rgb = pixels.get();

  constexpr int target = 224;
  const auto x_taps = BilinearTaps(width, target);
  const auto y_taps = BilinearTaps(height, target);
  std::vector<std::uint8_t> horizontal(static_cast<std::size_t>(target) * height * 3);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < target; ++x)
      for (int channel = 0; channel < 3; ++channel) {
        double value = 0;
        for (const auto& tap : x_taps[x])
          value += rgb[(static_cast<std::size_t>(y) * width + tap.index) * 3 + channel] * tap.weight;
        horizontal[(static_cast<std::size_t>(y) * target + x) * 3 + channel] = RoundByte(value);
      }
  std::vector<std::uint8_t> resized(static_cast<std::size_t>(target) * target * 3);
  for (int y = 0; y < target; ++y)
    for (int x = 0; x < target; ++x)
      for (int channel = 0; channel < 3; ++channel) {
        double value = 0;
        for (const auto& tap : y_taps[y])
          value += horizontal[(static_cast<std::size_t>(tap.index) * target + x) * 3 + channel] * tap.weight;
        resized[(static_cast<std::size_t>(y) * target + x) * 3 + channel] = RoundByte(value);
      }
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = target;
  image.height = target;
  image.format = PNG_FORMAT_RGB;
  png_alloc_size_t size = 0;
  if (!png_image_write_to_memory(&image, nullptr, &size, 0, resized.data(), 0, nullptr))
    throw std::runtime_error("PNG sizing failed");
  result.png.resize(size);
  if (!png_image_write_to_memory(&image, result.png.data(), &size, 0, resized.data(), 0, nullptr))
    throw std::runtime_error("PNG encoding failed");
  result.png.resize(size);
  result.inference_sha256 = Sha256(result.png.data(), result.png.size());
  return result;
}
}  // namespace aibox::siglip
