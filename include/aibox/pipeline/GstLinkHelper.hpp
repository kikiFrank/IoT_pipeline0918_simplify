#pragma once

#include <string>
#include <vector>
#include <gst/gst.h>

namespace aibox::pipeline {

bool LinkElements(const std::vector<GstElement*>& chain, std::string* out_error);
bool LinkPadToElement(GstPad* src_pad, GstElement* sink_elem,
                      const std::string& sink_pad_name, std::string* out_error);

}  // namespace aibox::pipeline
