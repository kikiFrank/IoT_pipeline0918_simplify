/*
 * YOLO26 end-to-end post-processing module.
 *
 * Output tensor: [1, 300, 6] float32
 * Per-row layout: [x1, y1, x2, y2, confidence, class_id]
 * Coordinates are absolute pixels in 640x640 input space (xyxy).
 * No NMS required (end-to-end model with built-in TopK + dedup).
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <string>

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
  void TransformDimensions(ObjectDetection &box, const Region& region);

  LogCallback  logger_;
  double       threshold_;
  LabelsParser labels_parser_;
};
