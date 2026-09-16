#include "aibox/pipeline/GstLinkHelper.hpp"
#include <sstream>

namespace aibox::pipeline {

bool LinkElements(const std::vector<GstElement*>& chain, std::string* out_error) {
  if (chain.size() < 2) return true;
  for (size_t i = 0; i + 1 < chain.size(); ++i) {
    if (!gst_element_link(chain[i], chain[i+1])) {
      if (out_error) {
        std::ostringstream oss;
        oss << "failed to link " << GST_ELEMENT_NAME(chain[i])
            << " -> " << GST_ELEMENT_NAME(chain[i+1]);
        *out_error = oss.str();
      }
      return false;
    }
  }
  return true;
}

bool LinkPadToElement(GstPad* src_pad, GstElement* sink_elem,
                      const std::string& sink_pad_name, std::string* out_error) {
  GstPad* sink = gst_element_get_static_pad(sink_elem, sink_pad_name.c_str());
  if (!sink) {
    if (out_error) *out_error = "sink pad not found: " + sink_pad_name;
    return false;
  }
  GstPadLinkReturn ret = gst_pad_link(src_pad, sink);
  gst_object_unref(sink);
  if (ret != GST_PAD_LINK_OK) {
    if (out_error) *out_error = "pad link failed to " + sink_pad_name;
    return false;
  }
  return true;
}

}  // namespace aibox::pipeline
