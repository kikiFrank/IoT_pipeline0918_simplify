/*
 * YOLO26 OBB (oriented bounding box) post-processing module.
 *
 * Expects a single output tensor output0 [1, 300, 7].
 * Per-row layout: [x_center, y_center, width, height, confidence, class_id,
 * angle] in 640x640 network-input pixel space, angle in radians.
 * The OBB is down-converted to its axis-aligned enclosing box (AABB) and then
 * letterbox-inverse-transformed to [0,1] relative to the source frame, so the
 * existing ROI meta / overlay / NATS chain works unchanged.
 *
 * DTYPE NOTE (measured on board):
 *   The DLC's native output0 is FLOAT16. The inference element used with this
 *   module is qtimlsnpe, which stores each fp16 value widened into a 32-bit
 *   slot (8400 bytes/frame = 300x7 fp32 slots holding fp16-representable
 *   values) and negotiates FLOAT32 transport caps. Tensor::type therefore
 *   reports kFloat32 and the data must be read as `const float*`.
 *   Caps() stays FLOAT32 [1,300,7] so the ml-postprocess caps check
 *   intersects qtimlsnpe's {FLOAT32,FLOAT16} src caps on FLOAT32.
 *
 * The model is end-to-end TopK. No NMS is performed here, by design.
 */

#include "ml-postprocess-yolo26-obb.h"

#include <algorithm>
#include <cmath>

static const float kDefaultThreshold = 0.40;

// Class ids for the 4 andon classes trained in this model.
static const int kMinClassId = 0;
static const int kMaxClassId = 3;

// Per-frame stats are not printed unconditionally: once at the first frame,
// then every kSummaryIntervalFrames, plus immediately whenever a frame shows
// anomalies (non-finite values, bad sizes or out-of-range classes).
static const uint32_t kSummaryIntervalFrames = 150;

static const std::string kModuleCaps = R"(
{
  "type": "object-detection",
  "tensors": [
    {
      "format": ["FLOAT32"],
      "dimensions": [
        [1, 300, 7]
      ]
    }
  ]
}
)";

Module::Module(LogCallback cb)
    : logger_(cb),
      threshold_(kDefaultThreshold),
      first_row_logged_(false),
      first_result_logged_(false),
      frames_since_summary_(kSummaryIntervalFrames) {
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
    LOG(logger_, kError, "YOLO26-OBB: failed to parse labels");
    return false;
  }

  if (!json_settings.empty()) {
    auto root = JsonValue::Parse(json_settings);

    if (!root || root->GetType() != JsonType::Object) {
      LOG(logger_, kError, "YOLO26-OBB: invalid JSON settings");
      return false;
    }

    if (root->GetObject().count("confidence"))
      threshold_ = root->GetNumber("confidence") / 100.0;

    LOG(logger_, kLog, "YOLO26-OBB config: threshold=%.2f", threshold_);
  }

  return true;
}

bool Module::Process(const Tensors& tensors, Dictionary& mlparams,
                     std::any& output) {

  if (output.type() != typeid(ObjectDetections)) {
    LOG(logger_, kError, "YOLO26-OBB: unexpected output type");
    return false;
  }

  ObjectDetections& detections =
      std::any_cast<ObjectDetections&>(output);

  /* ----- V1: exactly one tensor ----- */
  if (tensors.size() != 1) {
    LOG(logger_, kError,
        "YOLO26-OBB expects exactly 1 tensor (output0), got %u",
        static_cast<unsigned>(tensors.size()));
    return false;
  }

  /* ----- V2: negotiated transport type must be FLOAT32 -----
   * qtimlqnn always negotiates FLOAT32 regardless of the bin's native output
   * dtype; this guards the transport contract, not the payload layout. */
  if (tensors[0].type != TensorType::kFloat32) {
    LOG(logger_, kError,
        "YOLO26-OBB output0 transport type must be FLOAT32, got type=%u",
        static_cast<unsigned>(tensors[0].type));
    return false;
  }

  /* ----- V3: dimensions must be exactly [1, 300, 7] ----- */
  const std::vector<uint32_t>& d = tensors[0].dimensions;
  if (d.size() != 3 || d[0] != 1 || d[1] != 300 || d[2] != 7) {
    LOG(logger_, kError,
        "YOLO26-OBB output0 must be [1,300,7], got ndim=%u"
        " dims=[%u,%u,%u,%u]",
        static_cast<unsigned>(d.size()),
        d.size() > 0 ? d[0] : 0u, d.size() > 1 ? d[1] : 0u,
        d.size() > 2 ? d[2] : 0u, d.size() > 3 ? d[3] : 0u);
    return false;
  }

  /* ----- V4: data pointer ----- */
  if (tensors[0].data == nullptr) {
    LOG(logger_, kError, "YOLO26-OBB: null output0 data");
    return false;
  }

  /* Tensor name is soft-checked only: downstream ML meta may report
   * "unknown" for a perfectly valid tensor. Warn, never fail. */
  if (tensors[0].name != "output0" && tensors[0].name != "unknown") {
    LOG(logger_, kLog,
        "YOLO26-OBB WARN: tensor name '%s' != 'output0' (continuing)",
        tensors[0].name.c_str());
  }

  /* ----- V5: defensive input-tensor-region lookup ----- */
  auto region_it = mlparams.find("input-tensor-region");
  if (region_it == mlparams.end()) {
    LOG(logger_, kError, "YOLO26-OBB: missing 'input-tensor-region' in mlparams");
    return false;
  }
  const Region* region_ptr = std::any_cast<Region>(&region_it->second);
  if (region_ptr == nullptr) {
    LOG(logger_, kError, "YOLO26-OBB: 'input-tensor-region' is not a Region");
    return false;
  }
  const Region& region = *region_ptr;
  if (region.width == 0 || region.height == 0) {
    LOG(logger_, kError, "YOLO26-OBB: degenerate region %ux%u",
        region.width, region.height);
    return false;
  }

  const uint32_t n_rows = d[1];  /* 300 */
  const uint32_t stride = d[2];  /* 7   */

  /* Native payload is FLOAT16 but the DLC inference element (qtimlsnpe)
   * stores each fp16 value widened into a 32-bit slot (measured: 8400
   * bytes/frame = 300x7 fp32 slots holding fp16-representable values).
   * The negotiated transport type is FLOAT32, so read as float. */
  const float* data = static_cast<const float*>(tensors[0].data);

  /* One-time raw dump of the first few rows: fastest way to spot a column
   * order / unit mismatch. Emitted once for the lifetime of the process.
   * TEMP-DIAG: widened from 1 to 6 rows to identify the column layout. */
  if (!first_row_logged_) {
    first_row_logged_ = true;
    for (uint32_t dbg = 0; dbg < 6 && dbg < n_rows; dbg++) {
      const float* r = data + dbg * stride;
      LOG(logger_, kLog,
          "YOLO26-OBB row%u raw: [%.3f %.3f %.3f %.3f %.3f %.3f %.3f]"
          " name='%s' dims=[1,%u,%u]",
          dbg,
          r[0], r[1], r[2], r[3], r[4], r[5], r[6],
          tensors[0].name.c_str(), n_rows, stride);
    }
    /* TEMP-DIAG: raw fp32 bit patterns of the first 8 rows (stride 7), so the
     * true memory layout can be identified offline. */
    {
      const uint16_t* raw_bits = reinterpret_cast<const uint16_t*>(data);
      char hexbuf[1024];
      int off = 0;
      for (uint32_t k = 0; k < 56 && off < 900; k++) {
        off += snprintf(hexbuf + off, sizeof(hexbuf) - off, "%04x ", raw_bits[k]);
      }
      LOG(logger_, kLog, "YOLO26-OBB raw u16[0..55]: %s", hexbuf);
    }
    /* TEMP-DIAG: per-column min/max and count within [0,1] over all rows,
     * to identify which column carries the confidence. */
    for (uint32_t c = 0; c < stride; c++) {
      float lo = 1e30f, hi = -1e30f;
      uint32_t in01 = 0;
      for (uint32_t i = 0; i < n_rows; i++) {
        const float v = static_cast<float>(data[i * stride + c]);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (v >= 0.0f && v <= 1.0f) in01++;
      }
      LOG(logger_, kLog,
          "YOLO26-OBB col%u stats: min=%.4f max=%.4f in[0,1]=%u/%u",
          c, lo, hi, in01, n_rows);
    }
  }

  uint32_t n_low_conf  = 0;
  uint32_t n_nonfinite = 0;
  uint32_t n_bad_size  = 0;
  uint32_t n_bad_class = 0;

  for (uint32_t idx = 0; idx < n_rows; idx++) {
    const float* row = data + idx * stride;

    const float cx       = row[0];
    const float cy       = row[1];
    const float w        = row[2];
    const float h        = row[3];
    const float conf     = row[4];
    const float class_f  = row[5];
    const float angle    = row[6];

    /* V6: all seven values finite (angle included). */
    if (!std::isfinite(cx) || !std::isfinite(cy) ||
        !std::isfinite(w)  || !std::isfinite(h)  ||
        !std::isfinite(conf) || !std::isfinite(class_f) ||
        !std::isfinite(angle)) {
      n_nonfinite++;
      continue;
    }

    /* V7: confidence threshold. */
    if (conf < static_cast<float>(threshold_)) {
      n_low_conf++;
      continue;
    }

    /* V8: width/height must be positive. */
    if (w <= 0.0f || h <= 0.0f) {
      n_bad_size++;
      continue;
    }

    /* V9: class_id must be an integer in [0, 3].
     * Doubles as a column-order sentinel: a [.., class, conf, ..] layout would
     * make class_f fractional and be rejected here. */
    const float class_round = std::round(class_f);
    if (std::fabs(class_f - class_round) > 0.01f ||
        static_cast<int>(class_round) < kMinClassId ||
        static_cast<int>(class_round) > kMaxClassId) {
      n_bad_class++;
      continue;
    }
    const uint32_t class_id = static_cast<uint32_t>(static_cast<int>(class_round));

    /* V10: OBB -> AABB (axis-aligned enclosing box), 640x640 input space.
     *   aabb_w = |w*cos(a)| + |h*sin(a)|
     *   aabb_h = |w*sin(a)| + |h*cos(a)|
     * The absolute values make this independent of the angle sign /
     * clockwise-vs-counter-clockwise convention. */
    const float ca = std::fabs(std::cos(angle));
    const float sa = std::fabs(std::sin(angle));
    const float aabb_w = w * ca + h * sa;
    const float aabb_h = w * sa + h * ca;

    float left   = cx - aabb_w * 0.5f;
    float top    = cy - aabb_h * 0.5f;
    float right  = cx + aabb_w * 0.5f;
    float bottom = cy + aabb_h * 0.5f;

    /* V11: AABB must be finite and non-degenerate. */
    if (!std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom) ||
        right <= left || bottom <= top) {
      n_bad_size++;
      continue;
    }

    /* V12: clamp into the letterbox-filled region of the tensor. */
    ObjectDetection entry;
    entry.left   = std::max(left,   static_cast<float>(region.x));
    entry.top    = std::max(top,    static_cast<float>(region.y));
    entry.right  = std::min(right,  static_cast<float>(region.x + region.width));
    entry.bottom = std::min(bottom, static_cast<float>(region.y + region.height));

    /* V13: letterbox inverse transform -> [0,1] relative to source frame. */
    TransformDimensions(entry, region);

    entry.left   = std::clamp(entry.left,   0.0f, 1.0f);
    entry.top    = std::clamp(entry.top,    0.0f, 1.0f);
    entry.right  = std::clamp(entry.right,  0.0f, 1.0f);
    entry.bottom = std::clamp(entry.bottom, 0.0f, 1.0f);

    /* V14: reject boxes that collapsed after transform/clamp. */
    if (entry.right <= entry.left || entry.bottom <= entry.top) {
      n_bad_size++;
      continue;
    }

    /* V15: plugin convention - confidence is a percentage. */
    entry.confidence = conf * 100.0f;
    entry.name  = labels_parser_.GetLabel(class_id);
    entry.color = labels_parser_.GetColor(class_id);

    if (!first_result_logged_) {
      first_result_logged_ = true;
      LOG(logger_, kLog,
          "YOLO26-OBB first result: label=%s conf=%.2f "
          "obb=[cx=%.1f cy=%.1f w=%.1f h=%.1f angle=%.4f] "
          "aabb_norm=[%.4f %.4f %.4f %.4f]",
          entry.name.c_str(), entry.confidence,
          cx, cy, w, h, angle,
          entry.left, entry.top, entry.right, entry.bottom);
    }

    detections.emplace_back(std::move(entry));
  }

  /* Rate-limited stats: first frame, every kSummaryIntervalFrames, and
   * immediately on any anomaly. Never unconditional per frame. */
  const bool anomalous = (n_nonfinite > 0 || n_bad_size > 0 || n_bad_class > 0);
  ++frames_since_summary_;
  if (anomalous || frames_since_summary_ >= kSummaryIntervalFrames) {
    frames_since_summary_ = 0;
    LOG(logger_, kLog,
        "YOLO26-OBB: rows=%u kept=%u low_conf=%u nonfinite=%u bad_size=%u "
        "bad_class=%u thr=%.2f%s",
        n_rows, static_cast<unsigned>(detections.size()),
        n_low_conf, n_nonfinite, n_bad_size, n_bad_class, threshold_,
        anomalous ? " [ANOMALY]" : "");
  }

  return true;
}

IModule* NewModule(LogCallback logger) {
  return new Module(logger);
}
