#pragma once

#include <gst/gst.h>
#include "aibox/common/Types.hpp"

namespace aibox::pipeline {

class GstDotDumper {
 public:
  Status DumpDot(GstElement* pipeline, const FilePath& dot_path) const;
};

}  // namespace aibox::pipeline
