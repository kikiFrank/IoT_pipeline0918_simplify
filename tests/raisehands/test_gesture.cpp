// Test the actual official module and existing ImageNet input encoder on 165.
#include "../../plugins/mlpostprocess_roi/modules/image-classification/ml-postprocess-mobilenet-softmax.cc"
#include "aibox/pipeline/GstElementHelper.hpp"
#include <gst/gst.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 2) return 2;
  const GstVideoRectangle original{100, 100, 100, 200};
  const auto expanded = aibox::pipeline::ExpandRoiRectangle(original, 1920, 1080, 0.05);
  Check(expanded.x == 95 && expanded.y == 90 && expanded.w == 110 && expanded.h == 220,
        "five percent on each side");
  Check(original.x == 100 && original.y == 100 && original.w == 100 && original.h == 200,
        "original visible detection unchanged");
  const auto clipped = aibox::pipeline::ExpandRoiRectangle({0, 0, 100, 200}, 102, 204, 0.05);
  Check(clipped.x == 0 && clipped.y == 0 && clipped.w == 102 && clipped.h == 204,
        "frame boundary clipping");
  const auto identity = aibox::pipeline::ExpandRoiRectangle(original, 1920, 1080, 0);
  Check(identity.x == 100 && identity.y == 100 && identity.w == 100 && identity.h == 200,
        "default ratio preserves legacy pipeline");
  Module module([](uint32_t, const char*) {});
  Check(module.Configure(argv[1], R"({"confidence":0})"), "official softmax configure");
  for (const auto& values : {std::vector<float>{2, -2}, {-1, 1}, {27.8672f, -24.3958f}}) {
    Tensor tensor{};
    tensor.name = "logits"; tensor.type = kFloat32;
    tensor.dimensions = {1, 2}; tensor.data = const_cast<float*>(values.data());
    std::any output = ImageClassifications{};
    Dictionary params;
    Check(module.Process({tensor}, params, output), "official process");
    const auto& result = std::any_cast<const ImageClassifications&>(output);
    Check(result.size() == 2 && result[0].name == "Gesture" && result[1].name == "Normal", "class order");
    Check(std::isfinite(result[0].confidence) && std::isfinite(result[1].confidence), "finite probabilities");
    Check(std::abs(result[0].confidence + result[1].confidence - 100) < 0.001, "softmax sum");
    Check((result[0].confidence > result[1].confidence) == (values[0] > values[1]), "argmax unchanged");
  }
  constexpr double scale = 7.260096026584506e-05;
  constexpr int offset = -29172;
  constexpr double mean[] = {0.485, 0.456, 0.406};
  constexpr double sigma[] = {0.229, 0.224, 0.225};
  for (std::uint16_t pixel : {std::uint16_t{0}, std::uint16_t{65535}}) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, 128 * 128 * 3 * sizeof(pixel), nullptr);
    GstMapInfo map = GST_MAP_INFO_INIT;
    Check(gst_buffer_map(buffer, &map, GST_MAP_WRITE), "map input");
    std::fill_n(reinterpret_cast<std::uint16_t*>(map.data), 128 * 128 * 3, pixel);
    gst_buffer_unmap(buffer, &map);
    std::string error;
    Check(aibox::pipeline::EncodeColorInputInPlace(buffer, scale, offset, &error), "ImageNet encoder");
    Check(gst_buffer_map(buffer, &map, GST_MAP_READ), "read encoded tensor");
    const auto* encoded = reinterpret_cast<const std::uint16_t*>(map.data);
    for (size_t i = 0; i < 128 * 128 * 3; ++i) {
      const auto expected = static_cast<std::uint16_t>(std::clamp(std::round(
          (pixel / 65535.0 - mean[i % 3]) / sigma[i % 3] / scale - offset), 0.0, 65535.0));
      Check(encoded[i] == expected, "black padding / white pixel quantization");
    }
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
  }
  std::cout << "GESTURE_SOFTMAX_AND_INPUT_TEST_OK\n";
}
