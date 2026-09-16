/*
 * PP-YOLOE+ CRN-S post-processing module.
 *
 * Target model: ppyoloe_helmet_s_single_nonms_w8a16.bin
 *
 * Model contract used by this module:
 *   boxes  [1, 8400, 4]  xyxy
 *   scores [1, 4, 8400]  class-major
 *
 * IMPORTANT ABOUT DATATYPE:
 * The context BIN uses quantized UFIXED16 outputs, but qtimlqnn dequantizes
 * supported fixed-point outputs to FLOAT32 before handing tensors to the
 * post-processing module. Therefore this module intentionally reads both
 * tensor buffers as const float*.
 *
 * The NMS flow mirrors the validated Python PP-YOLOE non-NMS parser:
 *   for each class:
 *     score threshold -> TopK -> score sort -> greedy NMS
 *   merge all classes -> global score sort -> keep_top_k
 *
 * Unlike YOLO26-OTM, this module does NOT perform class argmax per anchor and
 * does NOT convert cxcywh to xyxy. PP-YOLOE boxes are already xyxy and one
 * anchor may legitimately contribute detections to more than one class.
 */

#include "ml-postprocess-ppyoloe.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kNumPredictions = 8400;
constexpr uint32_t kNumClasses = 4;

// Deployment-friendly default. The validated AP pipeline used 0.01 to build
// the full PR curve; set JSON "confidence": 1 if exact AP-eval behavior is
// desired. Configure() interprets confidence/nms values as percentages.
constexpr float kDefaultThreshold = 0.44f;
constexpr float kDefaultNmsThreshold = 0.70f;
constexpr uint32_t kDefaultMaxDetections = 300;
constexpr uint32_t kDefaultPreNmsTopK = 1000;

const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, 4, 8400],
        [1, 8400, 4]
      ]
    }
  ]
}
)";

}  // namespace

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold),
      nms_threshold_(kDefaultNmsThreshold),
      max_detections_(kDefaultMaxDetections),
      pre_nms_topk_(kDefaultPreNmsTopK) {
}

void Module::TransformDimensions(ObjectDetection& box,
                                 const Region& region) {
  box.top = (box.top - region.y) / region.height;
  box.bottom = (box.bottom - region.y) / region.height;
  box.left = (box.left - region.x) / region.width;
  box.right = (box.right - region.x) / region.width;
}

std::string Module::Caps() {
  return kModuleCaps;
}

bool Module::Configure(const std::string& labels_file,
                       const std::string& json_settings) {
  if (!labels_parser_.LoadFromFile(labels_file)) {
    LOG(logger_, kError, "PP-YOLOE: failed to parse labels");
    return false;
  }

  if (!json_settings.empty()) {
    auto root = JsonValue::Parse(json_settings);
    if (!root || root->GetType() != JsonType::Object) {
      LOG(logger_, kError, "PP-YOLOE: invalid JSON settings");
      return false;
    }

    const auto& obj = root->GetObject();

    // Keep the same convention as the existing YOLO26 module:
    // confidence=44 means 0.44, nms=70 means 0.70.
    if (obj.count("confidence")) {
      threshold_ = root->GetNumber("confidence") / 100.0;
    }
    if (obj.count("nms")) {
      nms_threshold_ = root->GetNumber("nms") / 100.0;
    }
    if (obj.count("max-detections")) {
      max_detections_ = static_cast<uint32_t>(
          root->GetNumber("max-detections"));
    }
    if (obj.count("pre-nms-topk")) {
      pre_nms_topk_ = static_cast<uint32_t>(
          root->GetNumber("pre-nms-topk"));
    }
  }

  if (threshold_ < 0.0 || threshold_ > 1.0) {
    LOG(logger_, kError,
        "PP-YOLOE: confidence threshold out of range: %.6f",
        threshold_);
    return false;
  }

  if (nms_threshold_ < 0.0 || nms_threshold_ > 1.0) {
    LOG(logger_, kError,
        "PP-YOLOE: NMS threshold out of range: %.6f",
        nms_threshold_);
    return false;
  }

  LOG(logger_, kLog,
      "PP-YOLOE config: threshold=%.4f nms=%.4f max_det=%u topk=%u",
      threshold_, nms_threshold_, max_detections_, pre_nms_topk_);

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {
  if (output.type() != typeid(ObjectDetections)) {
    LOG(logger_, kError, "PP-YOLOE: unexpected output type");
    return false;
  }

  ObjectDetections& detections =
      std::any_cast<ObjectDetections&>(output);

  if (tensors.size() != 2) {
    LOG(logger_, kError,
        "PP-YOLOE expects exactly 2 tensors (boxes + scores), got %u",
        static_cast<unsigned>(tensors.size()));
    return false;
  }

  // Tensor order is not assumed. Identify each output by its exact shape.
  int boxes_idx = -1;
  int scores_idx = -1;

  for (size_t i = 0; i < tensors.size(); ++i) {
    if (MatchBoxesShape(tensors[i])) {
      boxes_idx = static_cast<int>(i);
    } else if (MatchScoresShape(tensors[i])) {
      scores_idx = static_cast<int>(i);
    }
  }

  if (boxes_idx < 0 || scores_idx < 0) {
    LOG(logger_, kError,
        "PP-YOLOE: cannot identify boxes [1,8400,4] / "
        "scores [1,4,8400]");
    return false;
  }

  // qtimlqnn-facing tensors must be FLOAT32 after fixed-point dequantization.
  // Keep this runtime check because using the wrong datatype here would make
  // the postprocessor reinterpret quantized bytes as float values.
  if (tensors[boxes_idx].type != TensorType::kFloat32 ||
      tensors[scores_idx].type != TensorType::kFloat32) {
    LOG(logger_, kError,
        "PP-YOLOE: expected FLOAT32 postprocess tensors, got boxes=%u scores=%u",
        static_cast<unsigned>(tensors[boxes_idx].type),
        static_cast<unsigned>(tensors[scores_idx].type));
    return false;
  }

  const float* boxes_data =
      static_cast<const float*>(tensors[boxes_idx].data);
  const float* scores_data =
      static_cast<const float*>(tensors[scores_idx].data);

  if (boxes_data == nullptr || scores_data == nullptr) {
    LOG(logger_, kError, "PP-YOLOE: null tensor data pointer");
    return false;
  }

  auto region_it = mlparams.find("input-tensor-region");
  if (region_it == mlparams.end()) {
    LOG(logger_, kError,
        "PP-YOLOE: missing 'input-tensor-region' in mlparams");
    return false;
  }

  const auto* region_ptr = std::any_cast<Region>(&region_it->second);
  if (region_ptr == nullptr) {
    LOG(logger_, kError,
        "PP-YOLOE: 'input-tensor-region' is not a Region");
    return false;
  }

  const Region& region = *region_ptr;
  if (region.width <= 0 || region.height <= 0) {
    LOG(logger_, kError,
        "PP-YOLOE: invalid input tensor region: x=%d y=%d w=%d h=%d",
        region.x, region.y, region.width, region.height);
    return false;
  }

  // The validated Python parser rejects an anchor when either the xyxy box is
  // invalid/non-finite OR any class score for that anchor is non-finite.
  std::vector<uint8_t> valid_anchor(kNumPredictions, 1U);

  for (uint32_t i = 0; i < kNumPredictions; ++i) {
    const float left = boxes_data[i * 4 + 0];
    const float top = boxes_data[i * 4 + 1];
    const float right = boxes_data[i * 4 + 2];
    const float bottom = boxes_data[i * 4 + 3];

    bool valid = std::isfinite(left) &&
                 std::isfinite(top) &&
                 std::isfinite(right) &&
                 std::isfinite(bottom) &&
                 right > left &&
                 bottom > top;

    if (valid) {
      for (uint32_t c = 0; c < kNumClasses; ++c) {
        const float score =
            scores_data[c * kNumPredictions + i];
        if (!std::isfinite(score)) {
          valid = false;
          break;
        }
      }
    }

    valid_anchor[i] = valid ? 1U : 0U;
  }

  std::vector<Candidate> all_kept;
  all_kept.reserve(kDefaultMaxDetections * kNumClasses);

  // IMPORTANT: no per-anchor class argmax here. This mirrors the validated
  // Python PP-YOLOE postprocess: each class scans all valid anchors.
  for (uint32_t class_id = 0; class_id < kNumClasses; ++class_id) {
    std::vector<Candidate> class_candidates;

    for (uint32_t i = 0; i < kNumPredictions; ++i) {
      if (!valid_anchor[i])
        continue;

      const float confidence =
          scores_data[class_id * kNumPredictions + i];

      if (confidence < threshold_)
        continue;

      class_candidates.push_back({
          boxes_data[i * 4 + 0],
          boxes_data[i * 4 + 1],
          boxes_data[i * 4 + 2],
          boxes_data[i * 4 + 3],
          confidence,
          class_id,
      });
    }

    if (class_candidates.empty())
      continue;

    // Python behavior: select highest scoring candidates per class, then sort
    // descending before NMS. stable_sort also gives deterministic tie handling.
    std::stable_sort(
        class_candidates.begin(), class_candidates.end(),
        [](const Candidate& a, const Candidate& b) {
          return a.confidence > b.confidence;
        });

    if (pre_nms_topk_ > 0 &&
        class_candidates.size() > pre_nms_topk_) {
      class_candidates.resize(pre_nms_topk_);
    }

    std::vector<Candidate> kept = NmsSingleClass(class_candidates);

    LOG(logger_, kTrace,
        "PP-YOLOE class=%u candidates=%u kept_after_nms=%u",
        class_id,
        static_cast<unsigned>(class_candidates.size()),
        static_cast<unsigned>(kept.size()));

    all_kept.insert(all_kept.end(), kept.begin(), kept.end());
  }

  if (all_kept.empty()) {
    LOG(logger_, kTrace, "PP-YOLOE: no detections after NMS");
    return true;
  }

  // Python behavior: merge all classes, sort globally by confidence, then
  // keep only keep_top_k / max-detections.
  std::stable_sort(
      all_kept.begin(), all_kept.end(),
      [](const Candidate& a, const Candidate& b) {
        return a.confidence > b.confidence;
      });

  if (max_detections_ > 0 && all_kept.size() > max_detections_) {
    all_kept.resize(max_detections_);
  }

  for (const auto& c : all_kept) {
    ObjectDetection entry;

    // Python accuracy path clips xyxy to the evaluated image bounds after NMS.
    // Preserve the existing module/outer-postprocess contract by using the
    // supplied input-tensor-region as those bounds before normalization.
    entry.left = std::max(
        c.left, static_cast<float>(region.x));
    entry.top = std::max(
        c.top, static_cast<float>(region.y));
    entry.right = std::min(
        c.right, static_cast<float>(region.x + region.width));
    entry.bottom = std::min(
        c.bottom, static_cast<float>(region.y + region.height));

    if (entry.right <= entry.left || entry.bottom <= entry.top)
      continue;

    TransformDimensions(entry, region);

    entry.left = std::clamp(entry.left, 0.0f, 1.0f);
    entry.top = std::clamp(entry.top, 0.0f, 1.0f);
    entry.right = std::clamp(entry.right, 0.0f, 1.0f);
    entry.bottom = std::clamp(entry.bottom, 0.0f, 1.0f);

    if (entry.right <= entry.left || entry.bottom <= entry.top)
      continue;

    entry.confidence = c.confidence * 100.0f;
    entry.name = labels_parser_.GetLabel(c.class_id);
    entry.color = labels_parser_.GetColor(c.class_id);

    LOG(logger_, kTrace,
        "PP-YOLOE label=%s class=%u confidence=%.2f "
        "box[top=%.4f left=%.4f bottom=%.4f right=%.4f]",
        entry.name.c_str(), c.class_id, entry.confidence,
        entry.top, entry.left, entry.bottom, entry.right);

    detections.emplace_back(std::move(entry));
  }

  LOG(logger_, kTrace,
      "PP-YOLOE: final detections=%u",
      static_cast<unsigned>(detections.size()));

  return true;
}

float Module::IoU(const Candidate& a, const Candidate& b) const {
  const float width =
      std::min(a.right, b.right) - std::max(a.left, b.left);
  if (width <= 0.0f)
    return 0.0f;

  const float height =
      std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  if (height <= 0.0f)
    return 0.0f;

  const float intersection = width * height;
  const float area_a =
      std::max(0.0f, a.right - a.left) *
      std::max(0.0f, a.bottom - a.top);
  const float area_b =
      std::max(0.0f, b.right - b.left) *
      std::max(0.0f, b.bottom - b.top);
  const float union_area = area_a + area_b - intersection;

  if (union_area <= 0.0f)
    return 0.0f;

  return intersection / union_area;
}

std::vector<Module::Candidate> Module::NmsSingleClass(
    const std::vector<Candidate>& candidates) const {
  // Input is already sorted by confidence descending.
  std::vector<Candidate> kept;
  kept.reserve(candidates.size());

  for (const auto& candidate : candidates) {
    bool suppress = false;

    for (const auto& selected : kept) {
      // Python nms_xyxy keeps entries where IoU <= threshold, therefore an
      // entry is suppressed only when IoU > threshold (not >= threshold).
      if (IoU(candidate, selected) > nms_threshold_) {
        suppress = true;
        break;
      }
    }

    if (!suppress)
      kept.push_back(candidate);
  }

  return kept;
}

bool Module::MatchBoxesShape(const Tensor& tensor) const {
  return tensor.dimensions.size() == 3 &&
         tensor.dimensions[0] == 1 &&
         tensor.dimensions[1] == kNumPredictions &&
         tensor.dimensions[2] == 4;
}

bool Module::MatchScoresShape(const Tensor& tensor) const {
  return tensor.dimensions.size() == 3 &&
         tensor.dimensions[0] == 1 &&
         tensor.dimensions[1] == kNumClasses &&
         tensor.dimensions[2] == kNumPredictions;
}

IModule* NewModule(LogCallback logger) {
  return new Module(logger);
}
