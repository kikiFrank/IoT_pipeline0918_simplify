/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Adapted from Qualcomm's ml-postprocess-yolov8 module. This project's YOLOX
 * adaptation reads decoded YOLOX row-major candidates, multiplies objectness
 * by class probability, and applies score-sorted class-wise NMS.
 */
#include "ml-postprocess-yolox-ppe.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr uint32_t kPredictions = 8400;
#ifdef AIBOX_YOLOX_STACKLIGHT
constexpr uint32_t kClasses = 1;
constexpr const char* kModuleName = "YOLOX-STACKLIGHT";
#else
constexpr uint32_t kClasses = 3;
constexpr const char* kModuleName = "YOLOX-PPE";
#endif

constexpr float kNmsIou = 0.65f;

const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32", "UINT16"],
      "dimensions": [[1, 8400, )" + std::to_string(5 + kClasses) + R"(]]
    }
  ]
}
)";
}  // namespace

Module::Module(LogCallback cb) : logger_(cb) {}

std::string Module::Caps() { return kModuleCaps; }

bool Module::Configure(const std::string& labels_file,
                       const std::string& json_settings) {
  if (!labels_parser_.LoadFromFile(labels_file) ||
      static_cast<uint32_t>(labels_parser_.Size()) != kClasses) {
    LOG(logger_, kError, "%s requires exactly %u labels", kModuleName, kClasses);
    return false;
  }
  if (!json_settings.empty()) {
    auto root = JsonValue::Parse(json_settings);
    if (!root || root->GetType() != JsonType::Object) {
      LOG(logger_, kError, "%s invalid settings JSON", kModuleName);
      return false;
    }
    const float confidence = static_cast<float>(root->GetNumber("confidence"));
    if (!std::isfinite(confidence) || confidence < 0.0f || confidence > 100.0f) {
      LOG(logger_, kError, "%s invalid confidence percentage", kModuleName);
      return false;
    }
    threshold_ = confidence / 100.0f;
  }
  LOG(logger_, kLog, "%s threshold=%f nms=%f", kModuleName, threshold_, kNmsIou);
  return true;
}

float Module::ReadValue(const Tensor& tensor, uint32_t index) const {
  if (tensor.type == TensorType::kFloat32)
    return static_cast<const float*>(tensor.data)[index];
  const auto raw = static_cast<const uint16_t*>(tensor.data)[index];
  // Cast before adding the negative QNN offset to avoid unsigned wraparound.
  return (static_cast<float>(raw) + tensor.qoffset) * tensor.qscale;
}

float Module::IntersectionScore(const Candidate& a, const Candidate& b) {
  const float width = std::min(a.right, b.right) - std::max(a.left, b.left);
  const float height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  if (width <= 0.0f || height <= 0.0f) return 0.0f;
  const float intersection = width * height;
  const float area_a = (a.right - a.left) * (a.bottom - a.top);
  const float area_b = (b.right - b.left) * (b.bottom - b.top);
  return intersection / (area_a + area_b - intersection);
}

void Module::TransformDimensions(ObjectDetection& box, const Region& region) {
  box.top = (box.top - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left = (box.left - region.x) / region.width;
  box.right = (box.right - region.x) / region.width;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {
  if (output.type() != typeid(ObjectDetections) || tensors.size() != 1) {
    LOG(logger_, kError, "%s expects one tensor and ObjectDetections output", kModuleName);
    return false;
  }
  const Tensor& tensor = tensors.front();
  const uint32_t columns = 5 + kClasses;
  if (tensor.name != "output" || tensor.dimensions != std::vector<uint32_t>{1, kPredictions, columns} ||
      (tensor.type != TensorType::kFloat32 && tensor.type != TensorType::kUint16) ||
      tensor.data == nullptr) {
    LOG(logger_, kError, "%s tensor contract mismatch: name=%s type=%u dims=%u",
        kModuleName, tensor.name.c_str(), static_cast<unsigned>(tensor.type),
        static_cast<unsigned>(tensor.dimensions.size()));
    return false;
  }
  if (tensor.type == TensorType::kUint16 &&
      (!std::isfinite(tensor.qscale) || tensor.qscale <= 0.0f ||
       !std::isfinite(tensor.qoffset))) {
    LOG(logger_, kError, "%s invalid UINT16 quantization metadata", kModuleName);
    return false;
  }
  if (!logged_tensor_) {
    LOG(logger_, kLog,
        "%s runtime tensor name=%s type=%s dims=[1,8400,%u] qscale=%.9g qoffset=%.9g",
        kModuleName, tensor.name.c_str(),
        tensor.type == TensorType::kFloat32 ? "FLOAT32" : "UINT16",
        columns, tensor.qscale, tensor.qoffset);
    logged_tensor_ = true;
  }

  const auto region_it = mlparams.find("input-tensor-region");
  const auto* region = region_it == mlparams.end()
      ? nullptr : std::any_cast<Region>(&region_it->second);
  if (!region || region->width <= 0 || region->height <= 0) {
    LOG(logger_, kError, "%s missing or invalid input-tensor-region", kModuleName);
    return false;
  }

  std::vector<Candidate> candidates;
  candidates.reserve(256);
  for (uint32_t i = 0; i < kPredictions; ++i) {
    const uint32_t base = i * columns;
    const float cx = ReadValue(tensor, base);
    const float cy = ReadValue(tensor, base + 1);
    const float width = ReadValue(tensor, base + 2);
    const float height = ReadValue(tensor, base + 3);
    const float objectness = ReadValue(tensor, base + 4);
    float class_probability = -std::numeric_limits<float>::infinity();
    uint32_t class_id = 0;
    for (uint32_t c = 0; c < kClasses; ++c) {
      const float probability = ReadValue(tensor, base + 5 + c);
      if (!std::isfinite(probability)) {
        class_probability = std::numeric_limits<float>::quiet_NaN();
        break;
      }
      if (probability > class_probability) {
        class_probability = probability;
        class_id = c;
      }
    }
    if (!std::isfinite(cx) || !std::isfinite(cy) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(objectness) || !std::isfinite(class_probability) ||
        width <= 0.0f || height <= 0.0f) continue;
    const float score = objectness * class_probability;
    if (!std::isfinite(score) || score < threshold_) continue;
    candidates.push_back({cx - width / 2.0f, cy - height / 2.0f,
                          cx + width / 2.0f, cy + height / 2.0f,
                          score, class_id});
  }

  std::stable_sort(candidates.begin(), candidates.end(),
      [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  std::vector<Candidate> kept;
  kept.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    bool suppressed = false;
    for (const auto& selected : kept) {
      if (candidate.class_id == selected.class_id &&
          IntersectionScore(candidate, selected) > kNmsIou) {
        suppressed = true;
        break;
      }
    }
    if (!suppressed) kept.push_back(candidate);
  }

  ObjectDetections& detections = std::any_cast<ObjectDetections&>(output);
  for (const auto& candidate : kept) {
    ObjectDetection entry;
    entry.left = std::max(candidate.left, static_cast<float>(region->x));
    entry.top = std::max(candidate.top, static_cast<float>(region->y));
    entry.right = std::min(candidate.right,
                           static_cast<float>(region->x + region->width));
    entry.bottom = std::min(candidate.bottom,
                            static_cast<float>(region->y + region->height));
    if (entry.right <= entry.left || entry.bottom <= entry.top) continue;
    TransformDimensions(entry, *region);
    entry.confidence = candidate.score * 100.0f;
    entry.name = labels_parser_.GetLabel(candidate.class_id);
    entry.color = labels_parser_.GetColor(candidate.class_id);
    detections.emplace_back(std::move(entry));
  }
  LOG(logger_, kTrace, "%s candidates=%u kept=%u final=%u", kModuleName,
      static_cast<unsigned>(candidates.size()), static_cast<unsigned>(kept.size()),
      static_cast<unsigned>(detections.size()));
  return true;
}

IModule* NewModule(LogCallback logger) { return new Module(logger); }
