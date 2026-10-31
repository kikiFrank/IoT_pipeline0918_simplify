#include <gst/gst.h>
#include <gst/ml/gstmlmeta.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <algorithm>

struct Context { std::string prefix; bool captured = false; };

static GstPadProbeReturn Capture(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto& ctx = *static_cast<Context*>(data);
  if (ctx.captured) return GST_PAD_PROBE_OK;
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer || gst_buffer_get_size(buffer) == 0) return GST_PAD_PROBE_OK;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  gchar* text = caps ? gst_caps_to_string(caps) : nullptr;
  std::printf("%s caps=%s bytes=%zu memories=%u\n", ctx.prefix.c_str(),
      text ? text : "MISSING", gst_buffer_get_size(buffer), gst_buffer_n_memory(buffer));
  g_free(text);
  if (caps) gst_caps_unref(caps);
  auto* meta = gst_buffer_get_ml_tensor_meta_id(buffer, 0);
  if (meta) {
    std::printf("%s name=%s type=%u qscale=%.17g qoffset=%.17g dims=",
        ctx.prefix.c_str(), meta->name ? g_quark_to_string(meta->name) : "MISSING",
        meta->type, meta->qscale, meta->qoffset);
    for (guint d = 0; d < std::min(meta->n_dimensions, static_cast<guint>(GST_ML_TENSOR_MAX_DIMS)); ++d)
      std::printf("%u,", meta->dimensions[d]);
    std::printf("\n");
  } else std::printf("%s metadata=MISSING\n", ctx.prefix.c_str());
  GstMapInfo mapped = GST_MAP_INFO_INIT;
  if (gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
    std::ofstream out(ctx.prefix + ".raw", std::ios::binary);
    out.write(reinterpret_cast<const char*>(mapped.data), mapped.size);
    gst_buffer_unmap(buffer, &mapped);
    ctx.captured = true;
  }
  return GST_PAD_PROBE_OK;
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (argc != 4) return 2;
  const std::string converter = "qtivtransform ! video/x-raw,format=RGB ! qtimlvconverter engine=gles image-disposition=centre subpixel-layout=reverse";
  const char* pattern=std::getenv("PPE_TEST_PATTERN");
  const std::string spec = "videotestsrc num-buffers=1 pattern=" + std::string(pattern?pattern:"black") + " ! video/x-raw,format=RGB,width=" +
      std::string(argv[1]) + ",height=" + argv[2] +
      " ! " + converter + " name=preprocess ! neural-network/tensors,type=UINT16 ! qtimlqnn name=qnn "
      "model=data/models/ppe/yolox_ppe_w8a16.bin backend=/usr/lib/libQnnHtp.so "
      "system=/usr/lib/libQnnSystem.so ! fakesink";
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(spec.c_str(), &error);
  if (error || !pipeline) { if (error) g_printerr("%s\n", error->message); return 3; }
  GstElement* qnn = gst_bin_get_by_name(GST_BIN(pipeline), "qnn");
  Context input{std::string(argv[3]) + "_input"}, output{std::string(argv[3]) + "_output"};
  Context source{std::string(argv[3]) + "_source"};
  GstElement* preprocess=gst_bin_get_by_name(GST_BIN(pipeline),"preprocess");
  GstPad* video=preprocess?gst_element_get_static_pad(preprocess,"sink"):nullptr;
  if(video)gst_pad_add_probe(video,GST_PAD_PROBE_TYPE_BUFFER,Capture,&source,nullptr);
  GstPad* sink = gst_element_get_static_pad(qnn, "sink");
  GstPad* src = gst_element_get_static_pad(qnn, "src");
  gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, Capture, &input, nullptr);
  gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, Capture, &output, nullptr);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS && input.captured && output.captured;
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    gchar* debug = nullptr; gst_message_parse_error(msg, &error, &debug);
    g_printerr("%s %s\n", error->message, debug ? debug : "");
    g_clear_error(&error); g_free(debug);
  }
  if (msg) gst_message_unref(msg);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink); gst_object_unref(src); gst_object_unref(qnn);
  gst_object_unref(bus); gst_object_unref(pipeline);
  if(video)gst_object_unref(video);
  if(preprocess)gst_object_unref(preprocess);
  return ok ? 0 : 1;
}
