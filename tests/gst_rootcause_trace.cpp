// Diagnostic-only GStreamer 1.24 tracer. Does not change events, buffers or states.
// Board: g++ -std=c++17 -shared -fPIC ... $(pkg-config --cflags --libs gstreamer-1.0)
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

struct GstRootcauseTracer { GstTracer parent; };
struct GstRootcauseTracerClass { GstTracerClass parent; };
G_DEFINE_TYPE(GstRootcauseTracer, gst_rootcause_tracer, GST_TYPE_TRACER)

namespace {
struct PadProgress { guint64 count = 0; gint64 entered = 0; gint64 logged = 0; };
std::mutex progress_mutex;
std::unordered_map<GstPad*, PadProgress> progress;

bool selected(GstPad* pad) {
  auto* parent = GST_OBJECT_PARENT(pad); // Borrowed; valid for the hook invocation.
  const char* name = parent ? GST_OBJECT_NAME(parent) : nullptr;
  if (!name) return false;
  for (const char* prefix : {"rtspsrc_cam_", "h264parse_cam_", "v4l2h264dec_cam_", "rtph264depay_cam_",
                             "mux_", "metamux_", "queue_terminal_", "enc_webrtc_"}) {
    if (g_str_has_prefix(name, prefix)) return true;
  }
  return false;
}

void on_push_pre(GObject*, GstClockTime, GstPad* pad, GstBuffer*) {
  if (!selected(pad)) return;
  const gint64 now = g_get_monotonic_time();
  guint64 count = 0;
  {
    std::lock_guard<std::mutex> lock(progress_mutex);
    auto& entry = progress[pad];
    entry.entered = now;
    ++entry.count;
    if (now - entry.logged >= 5 * G_TIME_SPAN_SECOND) {
      entry.logged = now;
      count = entry.count;
    }
  }
  if (count) g_printerr("[ROOTCAUSE] epoch_us=%" G_GINT64_FORMAT " progress=%s:%s count=%" G_GUINT64_FORMAT "\n",
      g_get_real_time(), GST_OBJECT_NAME(GST_OBJECT_PARENT(pad)), GST_OBJECT_NAME(pad), count);
}

void on_push_post(GObject*, GstClockTime, GstPad* pad, GstFlowReturn result) {
  if (!selected(pad)) return;
  const gint64 now = g_get_monotonic_time();
  gint64 duration = 0;
  {
    std::lock_guard<std::mutex> lock(progress_mutex);
    const auto entry = progress.find(pad);
    if (entry != progress.end()) duration = now - entry->second.entered;
  }
  if (duration > 500000 || result != GST_FLOW_OK) {
    g_printerr("[ROOTCAUSE] epoch_us=%" G_GINT64_FORMAT " push_done=%s:%s duration_us=%" G_GINT64_FORMAT " flow=%s\n",
        g_get_real_time(), GST_OBJECT_NAME(GST_OBJECT_PARENT(pad)), GST_OBJECT_NAME(pad),
        duration, gst_flow_get_name(result));
  }
}

bool important(GstEvent* event) {
  return GST_EVENT_TYPE(event) == GST_EVENT_EOS || GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START;
}
void on_event_pre(GObject*, GstClockTime, GstPad* pad, GstEvent* event) {
  if (!important(event)) return;
  gchar* path = gst_object_get_path_string(GST_OBJECT(pad));
  g_printerr("[ROOTCAUSE] epoch_us=%" G_GINT64_FORMAT " event=%s pad=%s seq=%u\n",
      g_get_real_time(), GST_EVENT_TYPE_NAME(event), path, gst_event_get_seqnum(event));
  g_free(path);
}

void on_message(GObject*, GstClockTime, GstElement* element, GstMessage* message) {
  const auto type = GST_MESSAGE_TYPE(message);
  const auto* structure = gst_message_get_structure(message);
  const bool timeout = structure && gst_structure_has_name(structure, "GstRTSPSrcTimeout");
  if (type != GST_MESSAGE_EOS && type != GST_MESSAGE_ERROR && !timeout) return;
  gchar* path = gst_object_get_path_string(GST_OBJECT(element));
  gchar* details = structure ? gst_structure_to_string(structure) : g_strdup("");
  g_printerr("[ROOTCAUSE] epoch_us=%" G_GINT64_FORMAT " message=%s element=%s src=%s details=%s\n",
      g_get_real_time(), GST_MESSAGE_TYPE_NAME(message), path,
      GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : "?", details);
  g_free(details);
  g_free(path);
}
} // namespace

static void gst_rootcause_tracer_class_init(GstRootcauseTracerClass*) {}
static void gst_rootcause_tracer_init(GstRootcauseTracer* self) {
  auto* tracer = GST_TRACER(self);
  gst_tracing_register_hook(tracer, "pad-push-pre", G_CALLBACK(on_push_pre));
  gst_tracing_register_hook(tracer, "pad-push-post", G_CALLBACK(on_push_post));
  gst_tracing_register_hook(tracer, "pad-push-event-pre", G_CALLBACK(on_event_pre));
  gst_tracing_register_hook(tracer, "element-post-message-pre", G_CALLBACK(on_message));
  g_printerr("[ROOTCAUSE] tracer_loaded epoch_us=%" G_GINT64_FORMAT "\n", g_get_real_time());
}
static gboolean plugin_init(GstPlugin* plugin) {
  return gst_tracer_register(plugin, "rootcause", gst_rootcause_tracer_get_type());
}
#ifndef PACKAGE
#define PACKAGE "aibox-diagnostic"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rootcause,
                  "Diagnostic EOS and progress trace", plugin_init, "1.0", "LGPL", "aibox", "https://gstreamer.freedesktop.org")
