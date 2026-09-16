/*
 * YOLO26 OBB (oriented bounding box) post-processing module.
 *
 * Target model: yolo26n_obb_fp16in_nhwc_matmul_b_sym_w16.dlc (unquantized)
 * Single output tensor:
 *   output0 [1, 300, 7] - native dtype FLOAT16, widened to 32-bit slots and
 *   negotiated/transported as FLOAT32 by qtimlsnpe (see the dtype note in the
 *   .cc file)
 * Per-row layout (fixed, never guessed at runtime):
 *   [x_center, y_center, width, height, confidence, class_id, angle]
 *   - x_center/y_center/width/height: pixels in the 640x640 network input space
 *   - confidence: 0~1
 *   - class_id: 0..3 (green / red / warning-light / yellow)
 *   - angle: radians (Ultralytics OBB convention)
 *
 * The model is end-to-end TopK. No NMS is performed here (see conversion notes:
 * the horizontal-box NMS recipes must not be applied blindly to this output).
 *
 * OBB is down-converted to its axis-aligned enclosing box (AABB) so the
 * existing ROI / overlay / NATS chain keeps working unchanged.
 */

#pragma once

#include "qti-ml-post-process.h"
#include "qti-labels-parser.h"

#include <cstdint>
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
  bool         first_row_logged_;
  bool         first_result_logged_;
  uint32_t     frames_since_summary_;
  LabelsParser labels_parser_;
};
