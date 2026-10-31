/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Shared decoded YOLOX implementation. Default build is PPE [1,8400,8];
 * AIBOX_YOLOX_STACKLIGHT builds the separate stacklight [1,8400,6] module.
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
    float score;
    uint32_t class_id;
  };

  static float IntersectionScore(const Candidate& a, const Candidate& b);
  static void TransformDimensions(ObjectDetection& box, const Region& region);
  float ReadValue(const Tensor& tensor, uint32_t index) const;

  LogCallback logger_;
  LabelsParser labels_parser_;
  float threshold_ = 0.001f;
  bool logged_tensor_ = false;
};
