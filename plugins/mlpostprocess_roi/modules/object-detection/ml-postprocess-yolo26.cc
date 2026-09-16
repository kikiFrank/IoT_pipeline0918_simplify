/*
 * YOLO26 end-to-end post-processing module.
 *
 * Expects a single output tensor [1, 300, 6] from qtimlqnn.
 * The bin's output0 native dtype is QNN_DATATYPE_FLOAT_16; the buffer reaches
 * this module as raw fp16 bytes (see read note in Process()), so it is read as
 * __fp16.
 * Per-row layout: [x1, y1, x2, y2, confidence, class_id]
 *   - x1/y1/x2/y2: absolute pixel coordinates in 640x640 input space (xyxy)
 *   - confidence: 0~1
 *   - class_id: integer class index (COCO 80)
 * Results are pre-sorted by confidence (descending) and already deduplicated;
 * no NMS is performed here.
 */

#include "ml-postprocess-yolo26.h"

#include <algorithm>
#include <cmath>

static const float kDefaultThreshold = 0.40;

static const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, 300, 6]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold) {
}

void Module::TransformDimensions(ObjectDetection &box,
                                 const Region& region) {
  box.top    = (box.top    - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left   = (box.left   - region.x) / region.width;
  box.right  = (box.right  - region.x) / region.width;
}

std::string Module::Caps() {
  return kModuleCaps;
}

bool Module::Configure(const std::string& labels_file,
                       const std::string& json_settings) {

  if (!labels_parser_.LoadFromFile(labels_file)) {
    LOG(logger_, kError, "Failed to parse labels");
    return false;
  }

  if (!json_settings.empty()) {
    auto root = JsonValue::Parse(json_settings);

    if (!root || root->GetType() != JsonType::Object)
      return false;

    threshold_ = root->GetNumber("confidence") / 100.0;
    LOG(logger_, kLog, "Threshold: %f", threshold_);
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  if (tensors.size() != 1) {
    LOG(logger_, kError, "YOLO26 expects exactly 1 tensor, got %u",
        static_cast<unsigned>(tensors.size()));
    return false;
  }

  if (output.type() != typeid(ObjectDetections)) {
    LOG(logger_, kError, "Unexpected output type!");
    return false;
  }

  ObjectDetections& detections =
      std::any_cast<ObjectDetections&>(output);

  Region& region =
      std::any_cast<Region&>(mlparams["input-tensor-region"]);

  const uint32_t n_detections = tensors[0].dimensions[1];
  const uint32_t stride = tensors[0].dimensions[2];

  // NOTE: output0 of this YOLO26 bin is QNN_DATATYPE_FLOAT_16. The system
  // qtimlqnn engine has no FLOAT16 branch in convert_to_float(), so the buffer
  // is left as raw fp16 bytes (and the negotiated caps type is forced to
  // FLOAT32). We therefore read the data as __fp16 directly. dimensions[] are
  // element counts (dtype-independent), so n_detections/stride stay 300/6.
  // If this model is ever regenerated with a FLOAT32 output0, switch this back
  // to `const float*`.
  const __fp16* data = static_cast<const __fp16*>(tensors[0].data);

  for (uint32_t idx = 0; idx < n_detections; idx++) {
    const __fp16* row = data + idx * stride;

    float confidence = static_cast<float>(row[4]);

    if (confidence < threshold_)
      continue;

    uint32_t class_idx = static_cast<uint32_t>(static_cast<float>(row[5]));

    ObjectDetection entry;
    entry.left   = static_cast<float>(row[0]);
    entry.top    = static_cast<float>(row[1]);
    entry.right  = static_cast<float>(row[2]);
    entry.bottom = static_cast<float>(row[3]);

    entry.left   = std::max(entry.left,   static_cast<float>(region.x));
    entry.top    = std::max(entry.top,    static_cast<float>(region.y));
    entry.right  = std::min(entry.right,  static_cast<float>(region.x + region.width));
    entry.bottom = std::min(entry.bottom, static_cast<float>(region.y + region.height));

    LOG(logger_, kLog, "Class: %u Confidence: %.2f Box[%f, %f, %f, %f]",
        class_idx, confidence, entry.top, entry.left, entry.bottom, entry.right);

    TransformDimensions(entry, region);

    entry.left   = std::clamp(entry.left,   0.0f, 1.0f);
    entry.top    = std::clamp(entry.top,    0.0f, 1.0f);
    entry.right  = std::clamp(entry.right,  0.0f, 1.0f);
    entry.bottom = std::clamp(entry.bottom, 0.0f, 1.0f);

    if (entry.right <= entry.left || entry.bottom <= entry.top)
      continue;

    entry.confidence = confidence * 100.0f;
    entry.name = labels_parser_.GetLabel(class_idx);
    entry.color = labels_parser_.GetColor(class_idx);

    LOG(logger_, kTrace, "Label: %s Confidence: %.2f Box[%f, %f, %f, %f]",
        entry.name.c_str(), entry.confidence, entry.top, entry.left,
        entry.bottom, entry.right);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

IModule* NewModule(LogCallback logger) {
  return new Module(logger);
}
