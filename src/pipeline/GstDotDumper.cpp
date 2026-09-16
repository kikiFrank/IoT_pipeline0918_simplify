#include "aibox/pipeline/GstDotDumper.hpp"
#include <fstream>

namespace aibox::pipeline {

Status GstDotDumper::DumpDot(GstElement* pipeline,
                               const FilePath& dot_path) const {
  if (!pipeline)
    return Status::Error("DOT_INVALID_PIPELINE", "pipeline is null");

  auto pos = dot_path.rfind('/');
  if (pos != std::string::npos) {
    std::string dir = dot_path.substr(0, pos);
    if (!dir.empty()) system(("mkdir -p \"" + dir + "\"").c_str());
  }

  gchar* dot = gst_debug_bin_to_dot_data(GST_BIN(pipeline),
                                          GST_DEBUG_GRAPH_SHOW_ALL);
  if (!dot)
    return Status::Error("DOT_EXPORT_FAILED", "gst_debug_bin_to_dot_data null");

  std::ofstream ofs(dot_path);
  if (!ofs) { g_free(dot); return Status::Error("DOT_FILE_OPEN", dot_path); }
  ofs << dot;
  g_free(dot);
  return ofs.good() ? Status::Ok()
                    : Status::Error("DOT_FILE_WRITE", dot_path);
}

}  // namespace aibox::pipeline
