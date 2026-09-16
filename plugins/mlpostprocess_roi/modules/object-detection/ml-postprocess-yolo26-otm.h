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
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <string>
#include <vector>

class Module : public IModule {
 public:
  Module(LogCallback cb);
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

  void TransformDimensions(ObjectDetection &box, const Region& region);

  float IoU(const Candidate& a, const Candidate& b) const;

  std::vector<Candidate> NmsClassWise(
      std::vector<Candidate> candidates) const;

  bool MatchTensorShape(const Tensor& tensor,
                        uint32_t channels,
                        uint32_t predictions) const;

  LogCallback  logger_;
  double       threshold_;
  double       nms_threshold_;
  uint32_t     max_detections_;
  uint32_t     pre_nms_topk_;
  LabelsParser labels_parser_;
};
