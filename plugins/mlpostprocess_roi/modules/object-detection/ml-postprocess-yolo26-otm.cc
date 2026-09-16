/*
 * YOLO26 OTM (one-to-many) post-processing module.
 *
 * Target model: yolo26_otm_w8a8_input_u8_output_fp16.bin
 * Two output tensors:
 *   boxes  [1, 4, 8400]  FP16, cxcywh in 640x640 input space
 *   scores [1, 9, 8400]  FP16, class-major sigmoid scores (9 classes)
 *
 * Post-process responsibilities:
 *   argmax over 9 classes, xywh→xyxy, confidence threshold,
 *   pre-NMS TopK, class-wise NMS, letterbox→relative coordinate transform.
 *
 * NOTE: output tensors are QNN_DATATYPE_FLOAT_16. The system qtimlqnn engine
 * has no FLOAT16 branch in convert_to_float(), so the buffer is left as raw
 * fp16 bytes (and the negotiated caps type is forced to FLOAT32). We therefore
 * read the data as __fp16 directly.
 */

#include "ml-postprocess-yolo26-otm.h"

#include <algorithm>
#include <cmath>

static const float    kDefaultThreshold      = 0.40;
static const float    kDefaultNmsThreshold   = 0.45;
static const uint32_t kDefaultMaxDetections  = 300;
static const uint32_t kDefaultPreNmsTopK     = 3000;

static const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, 4, 8400],
        [1, 9, 8400]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold),
      nms_threshold_(kDefaultNmsThreshold),
      max_detections_(kDefaultMaxDetections),
      pre_nms_topk_(kDefaultPreNmsTopK) {
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

    const auto& obj = root->GetObject();
    if (obj.count("confidence"))     threshold_      = root->GetNumber("confidence") / 100.0;
    if (obj.count("nms"))            nms_threshold_  = root->GetNumber("nms") / 100.0;
    if (obj.count("max-detections")) max_detections_ = static_cast<uint32_t>(root->GetNumber("max-detections"));
    if (obj.count("pre-nms-topk"))   pre_nms_topk_   = static_cast<uint32_t>(root->GetNumber("pre-nms-topk"));

    LOG(logger_, kLog,
        "YOLO26-OTM config: threshold=%.2f nms=%.2f max_det=%u topk=%u",
        threshold_, nms_threshold_, max_detections_, pre_nms_topk_);
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  if (output.type() != typeid(ObjectDetections)) {
    LOG(logger_, kError, "Unexpected output type!");
    return false;
  }

  ObjectDetections& detections =
      std::any_cast<ObjectDetections&>(output);

  /* ----- Tensor validation ----- */
  if (tensors.size() != 2) {
    LOG(logger_, kError,
        "YOLO26-OTM expects exactly 2 tensors (boxes + scores), got %u",
        static_cast<unsigned>(tensors.size()));
    return false;
  }

  /* ----- Identify boxes vs scores by shape ----- */
  int boxes_idx  = -1;
  int scores_idx = -1;

  for (size_t i = 0; i < tensors.size(); i++) {
    if (MatchTensorShape(tensors[i], 4, 8400)) {
      boxes_idx = static_cast<int>(i);
    } else if (MatchTensorShape(tensors[i], 9, 8400)) {
      scores_idx = static_cast<int>(i);
    }
  }

  if (boxes_idx < 0 || scores_idx < 0) {
    LOG(logger_, kError,
        "YOLO26-OTM: cannot identify boxes [1,4,8400] / scores [1,9,8400]");
    return false;
  }

  /* ----- Read raw FP16 data ----- */
  const __fp16* boxes_data =
      static_cast<const __fp16*>(tensors[boxes_idx].data);
  const __fp16* scores_data =
      static_cast<const __fp16*>(tensors[scores_idx].data);

  auto region_it = mlparams.find("input-tensor-region");
  if (region_it == mlparams.end()) {
    LOG(logger_, kError, "Missing 'input-tensor-region' in mlparams");
    return false;
  }
  const auto* region_ptr = std::any_cast<Region>(&region_it->second);
  if (!region_ptr) {
    LOG(logger_, kError, "'input-tensor-region' is not a Region");
    return false;
  }
  Region& region = const_cast<Region&>(*region_ptr);

  constexpr uint32_t kNumPredictions = 8400;
  constexpr uint32_t kNumClasses     = 9;

  /* ----- Parse candidates: argmax + xywh→xyxy + confidence filter ----- */
  std::vector<Candidate> candidates;

  for (uint32_t i = 0; i < kNumPredictions; i++) {
    /* Channel-major boxes: [1, 4, 8400] */
    float cx = static_cast<float>(boxes_data[0 * kNumPredictions + i]);
    float cy = static_cast<float>(boxes_data[1 * kNumPredictions + i]);
    float w  = static_cast<float>(boxes_data[2 * kNumPredictions + i]);
    float h  = static_cast<float>(boxes_data[3 * kNumPredictions + i]);

    /* Class-major scores: [1, 9, 8400] — argmax */
    uint32_t class_id = 0;
    float confidence = static_cast<float>(scores_data[0 * kNumPredictions + i]);

    for (uint32_t c = 1; c < kNumClasses; c++) {
      float value = static_cast<float>(scores_data[c * kNumPredictions + i]);
      if (value > confidence) {
        confidence = value;
        class_id = c;
      }
    }

    if (confidence < threshold_)
      continue;

    /* xywh → xyxy */
    float left   = cx - w / 2.0f;
    float top    = cy - h / 2.0f;
    float right  = cx + w / 2.0f;
    float bottom = cy + h / 2.0f;

    /* Discard invalid boxes */
    if (right <= left || bottom <= top)
      continue;
    if (!std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom))
      continue;

    candidates.push_back({left, top, right, bottom, confidence, class_id});
  }

  LOG(logger_, kLog, "YOLO26-OTM: %u candidates above threshold %.2f",
      static_cast<unsigned>(candidates.size()), threshold_);

  if (candidates.empty())
    return true;

  /* ----- Sort by confidence descending, apply pre-NMS TopK ----- */
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.confidence > b.confidence;
            });

  if (candidates.size() > pre_nms_topk_)
    candidates.resize(pre_nms_topk_);

  /* ----- Class-wise NMS ----- */
  candidates = NmsClassWise(std::move(candidates));

  LOG(logger_, kLog, "YOLO26-OTM: %u candidates after NMS",
      static_cast<unsigned>(candidates.size()));

  /* ----- Final sort + limit ----- */
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.confidence > b.confidence;
            });

  if (candidates.size() > max_detections_)
    candidates.resize(max_detections_);

  /* ----- Convert to ObjectDetection with Region transform ----- */
  for (const auto& c : candidates) {
    ObjectDetection entry;
    entry.left   = std::max(c.left,   static_cast<float>(region.x));
    entry.top    = std::max(c.top,    static_cast<float>(region.y));
    entry.right  = std::min(c.right,  static_cast<float>(region.x + region.width));
    entry.bottom = std::min(c.bottom, static_cast<float>(region.y + region.height));

    TransformDimensions(entry, region);

    entry.left   = std::clamp(entry.left,   0.0f, 1.0f);
    entry.top    = std::clamp(entry.top,    0.0f, 1.0f);
    entry.right  = std::clamp(entry.right,  0.0f, 1.0f);
    entry.bottom = std::clamp(entry.bottom, 0.0f, 1.0f);

    if (entry.right <= entry.left || entry.bottom <= entry.top)
      continue;

    entry.confidence = c.confidence * 100.0f;
    entry.name  = labels_parser_.GetLabel(c.class_id);
    entry.color = labels_parser_.GetColor(c.class_id);

    LOG(logger_, kTrace, "Label: %s Confidence: %.2f Box[%.3f, %.3f, %.3f, %.3f]",
        entry.name.c_str(), entry.confidence,
        entry.top, entry.left, entry.bottom, entry.right);

    detections.emplace_back(std::move(entry));
  }

  return true;
}

/* ---- Private helpers ---- */

float Module::IoU(const Candidate& a, const Candidate& b) const {
  float w = std::min(a.right, b.right) - std::max(a.left, b.left);
  if (w <= 0.0f) return 0.0f;
  float h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  if (h <= 0.0f) return 0.0f;
  float inter = w * h;
  float area_a = (a.right - a.left) * (a.bottom - a.top);
  float area_b = (b.right - b.left) * (b.bottom - b.top);
  return inter / (area_a + area_b - inter);
}

std::vector<Module::Candidate> Module::NmsClassWise(
    std::vector<Candidate> candidates) const {
  /* Candidates are already sorted by confidence descending.
   * Group by class_id, then greedy NMS per class. */

  // Find unique class IDs
  std::vector<uint32_t> class_ids;
  for (const auto& c : candidates) {
    if (std::find(class_ids.begin(), class_ids.end(), c.class_id)
        == class_ids.end()) {
      class_ids.push_back(c.class_id);
    }
  }

  std::vector<Candidate> kept_all;

  for (uint32_t cls : class_ids) {
    std::vector<Candidate> kept_cls;

    for (const auto& c : candidates) {
      if (c.class_id != cls)
        continue;

      bool suppress = false;
      for (const auto& k : kept_cls) {
        if (IoU(c, k) >= nms_threshold_) {
          suppress = true;
          break;
        }
      }

      if (!suppress)
        kept_cls.push_back(c);
    }

    kept_all.insert(kept_all.end(), kept_cls.begin(), kept_cls.end());
  }

  return kept_all;
}

bool Module::MatchTensorShape(const Tensor& tensor,
                              uint32_t channels,
                              uint32_t predictions) const {
  return tensor.dimensions.size() == 3 &&
         tensor.dimensions[0] == 1 &&
         tensor.dimensions[1] == channels &&
         tensor.dimensions[2] == predictions;
}

IModule* NewModule(LogCallback logger) {
  return new Module(logger);
}
