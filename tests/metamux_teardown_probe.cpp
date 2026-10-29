// Board-only native plugin reproducer. Intentionally leaves metadata absent.
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2 || (std::strcmp(argv[1], "plain") != 0 &&
                    std::strcmp(argv[1], "flush") != 0)) return 2;
  gst_init(nullptr, nullptr);
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "appsrc name=video is-live=true format=time ! queue ! "
      "qtimetamux name=mux mode=sync latency=2000000000 ! "
      "fakesink sync=false async=false "
      "appsrc name=meta is-live=true format=time "
      "caps=text/x-raw,format=utf8 ! queue ! mux.data_0", &error);
  if (error || !pipeline) {
    if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
    if (pipeline) gst_object_unref(pipeline);
    return 1;
  }
  GstElement* video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
  GstCaps* caps = gst_caps_from_string(
      "video/x-raw,format=NV12,width=32,height=32,framerate=0/1");
  gst_app_src_set_caps(GST_APP_SRC(video), caps);
  gst_caps_unref(caps);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) return 1;
  // Match the real postprocessor's serialized empty ObjectDetection list.
  GValue list = G_VALUE_INIT, value = G_VALUE_INIT, boxes = G_VALUE_INIT;
  g_value_init(&list, GST_TYPE_LIST);
  g_value_init(&value, GST_TYPE_STRUCTURE);
  g_value_init(&boxes, GST_TYPE_ARRAY);
  GstStructure* prediction = gst_structure_new("ObjectDetection",
      "timestamp", G_TYPE_UINT64, static_cast<guint64>(0),
      "sequence-index", G_TYPE_UINT, 0u,
      "sequence-num-entries", G_TYPE_UINT, 1u, nullptr);
  gst_structure_set_value(prediction, "bounding-boxes", &boxes);
  g_value_take_boxed(&value, prediction);
  gst_value_list_append_value(&list, &value);
  gchar* serialized = gst_value_serialize(&list);
  g_value_unset(&boxes); g_value_unset(&value); g_value_unset(&list);
  GstElement* metadata = gst_bin_get_by_name(GST_BIN(pipeline), "meta");
  const auto length = std::strlen(serialized) + 1;
  GstBuffer* first_metadata = gst_buffer_new_wrapped(serialized, length);
  GST_BUFFER_PTS(first_metadata) = 0;
  gst_app_src_push_buffer(GST_APP_SRC(metadata), first_metadata);
  gst_object_unref(metadata);
  for (int i = 0; i < 6; ++i) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, 32 * 32 * 3 / 2, nullptr);
    gst_buffer_memset(buffer, 0, 0, gst_buffer_get_size(buffer));
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(i) * 100 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 100 * GST_MSECOND;
    if (gst_app_src_push_buffer(GST_APP_SRC(video), buffer) != GST_FLOW_OK) return 1;
  }
  g_usleep(4 * G_USEC_PER_SEC);
  gst_app_src_end_of_stream(GST_APP_SRC(video));
  gst_object_unref(video);
  g_usleep(200 * 1000);
  std::cout << "METAMUX_NULL_BEGIN mode=" << argv[1] << std::endl;
  if (std::strcmp(argv[1], "flush") == 0) {
    GstElement* mux = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
    for (const char* name : {"sink", "data_0"}) {
      GstPad* pad = gst_element_get_static_pad(mux, name);
      const gboolean accepted = gst_pad_send_event(pad, gst_event_new_flush_start());
      std::cout << "FLUSH_START_ACCEPTED pad=" << name << " value=" << accepted << std::endl;
      gst_object_unref(pad);
    }
    gst_object_unref(mux);
  }
  const auto result = gst_element_set_state(pipeline, GST_STATE_NULL);
  GstState current = GST_STATE_VOID_PENDING;
  const auto settled = gst_element_get_state(pipeline, &current, nullptr, 3 * GST_SECOND);
  const bool ok = result != GST_STATE_CHANGE_FAILURE && settled != GST_STATE_CHANGE_FAILURE && current == GST_STATE_NULL;
  gst_object_unref(pipeline);
  std::cout << (ok ? "METAMUX_NULL_OK" : "METAMUX_NULL_FAILED") << std::endl;
  return ok ? 0 : 1;
}
