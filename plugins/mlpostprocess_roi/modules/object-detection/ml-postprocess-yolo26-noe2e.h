/*
 * YOLO26 No-E2E (raw head) post-processing module.
 *
 * Target model: yolo26s W8A16 HTP BIN (yolo26s_w8a16_1013.bin)
 * Single output tensor:
 *   output0  [1, 14, 8400]  FP16, channel-major
 *     rows 0..3   : cx, cy, w, h in 640x640 input space
 *     rows 4..13  : 10-class scores (sigmoid), argmax over the class axis
 *
 * Post-process responsibilities:
 *   argmax over classes, xywh->xyxy, confidence threshold,
 *   pre-NMS TopK, class-wise NMS, letterbox->relative coordinate transform.
 *
 * The NMS / TopK / region-transform logic is reused from
 * ml-postprocess-yolo26-otm.cc; the tensor layout handling differs because
 * this model exports a single merged [1,14,8400] tensor (boxes + scores)
 * instead of two separate tensors.
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
  uint32_t     num_classes_;
  LabelsParser labels_parser_;
};
