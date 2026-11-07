// Compile on 165; the vendor interface has header-defined helper functions.
#include "../../plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolox-ppe.cc"

#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  Module module([](uint32_t, const char*) {});
  Check(module.Caps().find("[1, 8400, 13]") != std::string::npos, "eight-class caps");
  Module incompatible([](uint32_t, const char*) {});
  Check(!incompatible.Configure(argv[2], R"({"confidence":25})"), "reject PPE labels");
  Check(module.Configure(argv[1], R"({"confidence":25,"nms-iou":0.65})"), "configure");
  const std::vector<std::string> labels{
      "Green", "Green-Yellow", "None", "Red", "Red-green", "Yellow", "Blue", "Yellow-Blue"};
  std::vector<float> values(8400 * 13, 0.0f);
  // All classes at the same location must survive class-wise NMS.
  for (size_t i = 0; i < labels.size(); ++i) {
    float* row = values.data() + i * 13;
    row[0] = 320; row[1] = 320; row[2] = 40; row[3] = 60;
    row[4] = 0.9f; row[5 + i] = 0.8f;
  }
  // Duplicate Green is suppressed, low-confidence Red rejected.
  std::copy_n(values.data(), 13, values.data() + 8 * 13);
  values[8 * 13 + 4] = 0.8f;
  std::copy_n(values.data() + 3 * 13, 13, values.data() + 9 * 13);
  values[9 * 13 + 4] = 0.01f;
  Tensor tensor{};
  tensor.name = "output"; tensor.type = kFloat32;
  tensor.dimensions = {1, 8400, 13}; tensor.data = values.data();
  Dictionary params;
  params["input-tensor-region"] = Region(0, 140, 640, 360);
  std::any output = ObjectDetections{};
  Check(module.Process({tensor}, params, output), "float process");
  const auto& boxes = std::any_cast<const ObjectDetections&>(output);
  Check(boxes.size() == labels.size(), "class NMS and threshold");
  for (size_t i = 0; i < labels.size(); ++i) {
    Check(boxes[i].name == labels[i], "model class order including None");
    Check(std::abs(boxes[i].confidence - 72.0f) < 0.001f, "product score, no softmax");
    Check(std::abs(boxes[i].top - 150.0f / 360.0f) < 0.0001f, "centre padding restoration");
  }
  tensor.type = kUint16;
  tensor.qscale = 0.010490993038f; tensor.qoffset = -4595.0f;
  std::vector<uint16_t> quantized(values.size());
  for (size_t i = 0; i < values.size(); ++i)
    quantized[i] = static_cast<uint16_t>(std::round(values[i] / tensor.qscale - tensor.qoffset));
  tensor.data = quantized.data(); output = ObjectDetections{};
  Check(module.Process({tensor}, params, output), "U16 process");
  Check(std::any_cast<const ObjectDetections&>(output).size() == 8, "new model encoding");
  tensor.dimensions = {1, 8400, 8};
  Check(!module.Process({tensor}, params, output), "reject PPE shape");
  tensor.dimensions = {1, 13, 8400};
  Check(!module.Process({tensor}, params, output), "reject transpose");
  std::cout << "COLOR8_POSTPROCESS_TEST_OK\n";
}
