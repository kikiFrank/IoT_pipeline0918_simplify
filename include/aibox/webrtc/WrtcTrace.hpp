#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <gst/gst.h>

namespace aibox::webrtc::trace {

inline std::mutex g_trace_mu;

// CP03 observation-only trace.  Keep fields compact and never put SDP, ICE
// candidates, NATS payloads, or model output in detail.
inline const char* StateName(GstState state) {
  switch (state) {
    case GST_STATE_VOID_PENDING: return "void";
    case GST_STATE_NULL: return "null";
    case GST_STATE_READY: return "ready";
    case GST_STATE_PAUSED: return "paused";
    case GST_STATE_PLAYING: return "playing";
  }
  return "unknown";
}

inline void Emit(const char* event, const std::string& camera_id,
                 const std::string& session_id, int session_generation,
                 int encoder_generation, size_t qdepth,
                 const char* state, const char* result,
                 const std::string& detail = {}, GstElement* element = nullptr) {
  GstState current = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  if (element) gst_element_get_state(element, &current, &pending, 0);

  std::ostringstream tid;
  tid << std::this_thread::get_id();
  std::ostringstream line;
  line << "WRTC_TRACE"
       << " mono_us=" << g_get_monotonic_time()
       << " tid=" << tid.str()
       << " ctx=" << (g_main_context_is_owner(g_main_context_default()) ? "owner" : "not-owner")
       << " cam=" << (camera_id.empty() ? "-" : camera_id)
       << " sid=" << (session_id.empty() ? "-" : session_id)
       << " gen=s" << session_generation << "/e" << encoder_generation
       << " event=" << event
       << " qdepth=" << qdepth
       << " state=" << state
       << " gst_cur=" << StateName(current)
       << " gst_pending=" << StateName(pending)
       << " result=" << result
       << " detail=" << (detail.empty() ? "-" : detail.substr(0, 96));
  std::lock_guard<std::mutex> lock(g_trace_mu);
  std::cerr << line.str() << std::endl;
}

}  // namespace aibox::webrtc::trace
