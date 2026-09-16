/*
 * PP-YOLOE+ CRN-S post-processing module.
 *
 * Target model: ppyoloe_helmet_s_single_nonms_w8a16.bin
 *
 * qtimlqnn-facing output tensors (after QNN dequantization):
 *   boxes  [1, 8400, 4] FLOAT32, xyxy in 640x640 network-input space
 *   scores [1, 4, 8400] FLOAT32, class-major scores
 *
 * Post-process responsibilities:
 *   - validate and identify the two output tensors by shape
 *   - preserve PP-YOLOE multi-class-per-anchor semantics
 *   - confidence threshold per class
 *   - per-class pre-NMS TopK
 *   - greedy class-wise NMS
 *   - global confidence sort + keep_top_k
 *   - convert model-space xyxy to normalized ObjectDetection coordinates
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <cstdint>
#include <string>
#include <vector>

class Module : public IModule {
 public:
  explicit Module(LogCallback cb);
  ~Module() {};

  std::string Caps() override;

  bool Configure(const std::string& labels_file,
                 const std::string& json_settings) override;

  bool Process(const Tensors& tensors, Dictionary& mlparams,
               std::any& output) override;

 private:
  struct Candidate {
    float left;
    float top;
    float right;
    float bottom;
    float confidence;
    uint32_t class_id;
  };

  void TransformDimensions(ObjectDetection& box, const Region& region);

  float IoU(const Candidate& a, const Candidate& b) const;

  std::vector<Candidate> NmsSingleClass(
      const std::vector<Candidate>& candidates) const;

  bool MatchBoxesShape(const Tensor& tensor) const;
  bool MatchScoresShape(const Tensor& tensor) const;

  LogCallback logger_;
  double threshold_;
  double nms_threshold_;
  uint32_t max_detections_;
  uint32_t pre_nms_topk_;
  LabelsParser labels_parser_;
};
