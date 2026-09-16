// aiboxbatchsplit unit tests. No real QNN. No credit.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef GST_PARENT_BUFFER_META_API_TYPE
extern "C" GstMeta *gst_buffer_add_parent_buffer_meta (GstBuffer *buffer,
                                                       GstBuffer *ref);
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

struct MiniPipe {
  GstElement *pipe = nullptr;
  GstElement *split = nullptr;
  GstElement *asrc = nullptr;
  GstElement *asink_packed = nullptr;
  GstElement *asink_v0 = nullptr;
  GstElement *asink_v1 = nullptr;
};

static GstCaps *mono_caps() {
  return gst_caps_from_string(
      "video/x-raw,format=GRAY8,width=8,height=8,framerate=25/1");
}

static GstCaps *batch_caps() {
  return gst_caps_from_string(
      "video/x-raw(memory:BatchMemory),format=GRAY8,width=8,height=8,framerate=25/1");
}

struct LocalFrameMeta {
  GstMeta meta;
  gchar *camera_id;
  gchar *algorithm_group_id;
  gchar *time_mux_group_id;
  guint mux_slot_id;
  guint64 frame_id;
  GstClockTime pts;
  guint global_cam_index;
  gchar *batch_group_id;
  guint batch_stream_id;
};

static void add_frame_meta(GstBuffer *buf, const char *cam, guint stream,
                           guint64 frame, GstClockTime pts) {
  const GstMetaInfo *mi = gst_meta_get_info("GstAiboxFrameMeta");
  CHECK(mi != nullptr);
  if (!mi) return;
  auto *m = (LocalFrameMeta *)gst_buffer_add_meta(buf, mi, nullptr);
  CHECK(m != nullptr);
  if (!m) return;
  m->camera_id = g_strdup(cam);
  m->algorithm_group_id = g_strdup("");
  m->time_mux_group_id = g_strdup("");
  m->mux_slot_id = 0;
  m->frame_id = frame;
  m->pts = pts;
  m->global_cam_index = stream;
  m->batch_group_id = g_strdup("g0");
  m->batch_stream_id = stream;
}

static GstBuffer *make_parent(const char *cam, guint stream, guint64 frame,
                              GstClockTime pts) {
  GstBuffer *b = gst_buffer_new_allocate(nullptr, 64, nullptr);
  GST_BUFFER_PTS(b) = pts;
  GST_BUFFER_DURATION(b) = 40 * GST_MSECOND;
  add_frame_meta(b, cam, stream, frame, pts);
  return b;
}

static GstBuffer *make_wrapper(guint mask, GstBuffer *p0, GstBuffer *p1) {
  GstBuffer *w = gst_buffer_new_allocate(nullptr, 128, nullptr);
  GST_BUFFER_OFFSET(w) = mask;
  GST_BUFFER_PTS(w) = p0 ? GST_BUFFER_PTS(p0) : 0;
  if (p0) gst_buffer_add_parent_buffer_meta(w, p0);
  if (p1) gst_buffer_add_parent_buffer_meta(w, p1);
  return w;
}

static MiniPipe make_pipe() {
  MiniPipe p;
  p.pipe = gst_pipeline_new("split-ut");
  p.split = gst_element_factory_make("aiboxbatchsplit", "split");
  p.asrc = gst_element_factory_make("appsrc", "asrc");
  p.asink_packed = gst_element_factory_make("appsink", "packed");
  p.asink_v0 = gst_element_factory_make("appsink", "v0");
  p.asink_v1 = gst_element_factory_make("appsink", "v1");
  CHECK(p.split && p.asrc && p.asink_packed && p.asink_v0 && p.asink_v1);
  if (!p.split) return p;

  GstCaps *mc = mono_caps();
  gchar *ms = gst_caps_to_string(mc);
  g_object_set(p.split, "video-caps", ms, "camera-0", "cam-001", "camera-1",
               "cam-002", nullptr);
  g_free(ms);
  gst_caps_unref(mc);

  GstCaps *bc = batch_caps();
  g_object_set(p.asrc, "is-live", FALSE, "format", GST_FORMAT_TIME, "block",
               FALSE, nullptr);
  gst_app_src_set_caps(GST_APP_SRC(p.asrc), bc);
  gst_caps_unref(bc);
  for (GstElement *s : {p.asink_packed, p.asink_v0, p.asink_v1})
    g_object_set(s, "sync", FALSE, "async", FALSE, "emit-signals", FALSE,
                 nullptr);

  gst_bin_add_many(GST_BIN(p.pipe), p.asrc, p.split, p.asink_packed, p.asink_v0,
                   p.asink_v1, nullptr);
  CHECK(gst_element_link_pads(p.asrc, "src", p.split, "sink"));
  CHECK(gst_element_link_pads(p.split, "video_0", p.asink_v0, "sink"));
  CHECK(gst_element_link_pads(p.split, "video_1", p.asink_v1, "sink"));
  CHECK(gst_element_link_pads(p.split, "src", p.asink_packed, "sink"));
  gst_element_set_state(p.pipe, GST_STATE_PLAYING);
  gst_element_get_state(p.pipe, nullptr, nullptr, GST_SECOND);
  return p;
}

static void destroy_pipe(MiniPipe *p) {
  if (!p->pipe) return;
  gst_element_set_state(p->pipe, GST_STATE_NULL);
  gst_element_get_state(p->pipe, nullptr, nullptr, GST_SECOND);
  gst_object_unref(p->pipe);
  memset(p, 0, sizeof(*p));
}

static guint64 get_u64(GstElement *e, const char *name) {
  guint64 v = 0;
  g_object_get(e, name, &v, nullptr);
  return v;
}

static GstSample *pull(GstElement *sink, guint timeout_ms) {
  return gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                      timeout_ms * GST_MSECOND);
}

static void push_mask(MiniPipe *p, guint mask, GstClockTime pts,
                      guint64 frame = 1) {
  GstBuffer *p0 = make_parent("cam-001", 0, frame, pts);
  GstBuffer *p1 = make_parent("cam-002", 1, frame, pts);
  GstBuffer *w = make_wrapper(mask, p0, p1);
  gst_buffer_unref(p0);
  gst_buffer_unref(p1);
  gst_app_src_push_buffer(GST_APP_SRC(p->asrc), w);
}

static GstBus *watch_bus(GstElement *pipe) {
  return gst_element_get_bus(pipe);
}

static bool bus_saw_error(GstBus *bus, guint timeout_ms) {
  GstMessage *m = gst_bus_timed_pop_filtered(
      bus, timeout_ms * GST_MSECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR));
  if (!m) return false;
  gst_message_unref(m);
  return true;
}

static void test_names() {
  GstElement *e = gst_element_factory_make("aiboxbatchsplit", "n");
  CHECK(e != nullptr);
  if (!e) return;
  CHECK(g_strcmp0(G_OBJECT_TYPE_NAME(e), "GstAiboxBatchSplit") == 0);
  gst_object_unref(e);
}

static void test_full_forks_video_and_src() {
  MiniPipe p = make_pipe();
  if (!p.split) return;
  push_mask(&p, 0x3, 0);
  GstSample *v0 = pull(p.asink_v0, 800);
  GstSample *v1 = pull(p.asink_v1, 800);
  GstSample *pk = pull(p.asink_packed, 800);
  CHECK(v0 && v1 && pk);
  if (v0) {
    GstCaps *c = gst_sample_get_caps(v0);
    gchar *s = c ? gst_caps_to_string(c) : g_strdup("");
    CHECK(strstr(s, "GRAY8") != nullptr);
    CHECK(strstr(s, "BatchMemory") == nullptr);
    g_free(s);
    gst_sample_unref(v0);
  }
  if (v1) gst_sample_unref(v1);
  if (pk) gst_sample_unref(pk);
  CHECK(get_u64(p.split, "forwarded") == 1);
  CHECK(get_u64(p.split, "drop-partial") == 0);
  destroy_pipe(&p);
}

static void test_partial_silent_drop() {
  MiniPipe p = make_pipe();
  if (!p.split) return;
  GstBus *bus = watch_bus(p.pipe);
  push_mask(&p, 0x1, 0);
  push_mask(&p, 0x2, GST_SECOND);
  CHECK(pull(p.asink_v0, 200) == nullptr);
  CHECK(pull(p.asink_v1, 50) == nullptr);
  CHECK(pull(p.asink_packed, 50) == nullptr);
  CHECK(get_u64(p.split, "drop-partial") == 2);
  CHECK(get_u64(p.split, "forwarded") == 0);
  CHECK(!bus_saw_error(bus, 100));
  gst_object_unref(bus);
  destroy_pipe(&p);
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  GstPlugin *fm = gst_plugin_load_by_name("aiboxframemeta");
  CHECK(fm != nullptr);
  if (fm) gst_object_unref(fm);
  GstElement *probe = gst_element_factory_make("aiboxbatchsplit", "probe");
  if (!probe) {
    fprintf(stderr, "aiboxbatchsplit factory missing; set GST_PLUGIN_PATH\n");
    return 2;
  }
  gst_object_unref(probe);

  test_names();
  test_full_forks_video_and_src();
  test_partial_silent_drop();

  if (g_failures) {
    fprintf(stderr, "test_aiboxbatchsplit FAIL count=%d\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_aiboxbatchsplit PASS\n");
  return 0;
}
