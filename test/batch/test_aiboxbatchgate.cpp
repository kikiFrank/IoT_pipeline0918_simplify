// Phase 3-R2: post-qtibatch aiboxbatchgate unit tests. No real QNN.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  GstElement *gate = nullptr;
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

static MiniPipe make_pipe(bool link_packed = true, bool link_v0 = true,
                          bool link_v1 = true) {
  MiniPipe p;
  p.pipe = gst_pipeline_new("gate-ut");
  p.gate = gst_element_factory_make("aiboxbatchgate", "gate");
  p.asrc = gst_element_factory_make("appsrc", "asrc");
  p.asink_packed = gst_element_factory_make("appsink", "packed");
  p.asink_v0 = gst_element_factory_make("appsink", "v0");
  p.asink_v1 = gst_element_factory_make("appsink", "v1");
  CHECK(p.gate && p.asrc && p.asink_packed && p.asink_v0 && p.asink_v1);
  if (!p.gate) return p;

  GstCaps *mc = mono_caps();
  gchar *ms = gst_caps_to_string(mc);
  g_object_set(p.gate, "video-caps", ms, "camera-0", "cam-001", "camera-1",
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

  gst_bin_add_many(GST_BIN(p.pipe), p.asrc, p.gate, p.asink_packed, p.asink_v0,
                   p.asink_v1, nullptr);
  CHECK(gst_element_link_pads(p.asrc, "src", p.gate, "sink"));
  if (link_v0) CHECK(gst_element_link_pads(p.gate, "video_0", p.asink_v0, "sink"));
  if (link_v1) CHECK(gst_element_link_pads(p.gate, "video_1", p.asink_v1, "sink"));
  if (link_packed)
    CHECK(gst_element_link_pads(p.gate, "src", p.asink_packed, "sink"));
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

static guint get_u(GstElement *e, const char *name) {
  guint v = 0;
  g_object_get(e, name, &v, nullptr);
  return v;
}

static gboolean emit_complete(GstElement *gate, guint mask, gboolean ok) {
  gboolean ret = FALSE;
  g_signal_emit_by_name(gate, "complete-batch", mask, ok, &ret);
  return ret;
}

static GstSample *pull(GstElement *sink, guint timeout_ms) {
  return gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                      timeout_ms * GST_MSECOND);
}

static void push_full(MiniPipe *p, GstClockTime pts, guint64 frame = 1) {
  GstBuffer *p0 = make_parent("cam-001", 0, frame, pts);
  GstBuffer *p1 = make_parent("cam-002", 1, frame, pts);
  GstBuffer *w = make_wrapper(0x3, p0, p1);
  gst_buffer_unref(p0);
  gst_buffer_unref(p1);
  gst_app_src_push_buffer(GST_APP_SRC(p->asrc), w);
}

static void test_names() {
  GstElement *e = gst_element_factory_make("aiboxbatchgate", "n");
  CHECK(e != nullptr);
  if (!e) return;
  CHECK(g_strcmp0(G_OBJECT_TYPE_NAME(e), "GstAiboxBatchGate") == 0);
  gst_object_unref(e);
}

static void test_admit_credit_and_caps() {
  MiniPipe p = make_pipe();
  if (!p.gate) return;
  CHECK(get_u(p.gate, "credit") == 1);
  push_full(&p, 0);
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
  if (pk) {
    GstCaps *c = gst_sample_get_caps(pk);
    gchar *s = c ? gst_caps_to_string(c) : g_strdup("");
    CHECK(strstr(s, "BatchMemory") != nullptr);
    g_free(s);
    GstBuffer *b = gst_sample_get_buffer(pk);
    CHECK(b && GST_BUFFER_OFFSET(b) == 0x3);
    gst_sample_unref(pk);
  }
  CHECK(get_u64(p.gate, "admitted-batches") == 1);
  CHECK(get_u(p.gate, "inflight") == 1);
  CHECK(get_u(p.gate, "credit") == 0);
  CHECK(emit_complete(p.gate, 0x3, TRUE) == TRUE);
  CHECK(get_u(p.gate, "credit") == 1);
  CHECK(get_u64(p.gate, "completed-batches") == 1);
  destroy_pipe(&p);
}

static void test_partial_no_credit() {
  MiniPipe p = make_pipe();
  if (!p.gate) return;
  GstBuffer *p0 = make_parent("cam-001", 0, 1, 0);
  GstBuffer *w = make_wrapper(0x1, p0, nullptr);
  gst_buffer_unref(p0);
  gst_app_src_push_buffer(GST_APP_SRC(p.asrc), w);
  g_usleep(80 * 1000);
  CHECK(get_u64(p.gate, "drop-partial") >= 1);
  CHECK(get_u64(p.gate, "admitted-batches") == 0);
  CHECK(get_u(p.gate, "credit") == 1);
  CHECK(pull(p.asink_packed, 50) == nullptr);
  destroy_pipe(&p);
}

static void test_invalid_and_overload() {
  MiniPipe p = make_pipe();
  if (!p.gate) return;
  GstBuffer *bad = gst_buffer_new_allocate(nullptr, 32, nullptr);
  GST_BUFFER_OFFSET(bad) = 0x3;
  gst_app_src_push_buffer(GST_APP_SRC(p.asrc), bad);
  g_usleep(50 * 1000);
  CHECK(get_u64(p.gate, "invalid-batch") >= 1);
  CHECK(get_u(p.gate, "credit") == 1);

  push_full(&p, 0, 1);
  GstSample *v0 = pull(p.asink_v0, 800);
  GstSample *v1 = pull(p.asink_v1, 800);
  GstSample *pk = pull(p.asink_packed, 800);
  if (v0) gst_sample_unref(v0);
  if (v1) gst_sample_unref(v1);
  if (pk) gst_sample_unref(pk);
  CHECK(get_u(p.gate, "inflight") == 1);

  push_full(&p, 40 * GST_MSECOND, 2);
  push_full(&p, 80 * GST_MSECOND, 3);
  g_usleep(80 * 1000);
  CHECK(get_u(p.gate, "pending-full-batch") <= 1);
  CHECK(get_u64(p.gate, "drop-overload") >= 1);
  CHECK(emit_complete(p.gate, 0x3, TRUE) == TRUE);
  GstSample *v0b = pull(p.asink_v0, 800);
  GstSample *v1b = pull(p.asink_v1, 800);
  GstSample *pkb = pull(p.asink_packed, 800);
  CHECK(v0b && v1b && pkb);
  if (v0b) gst_sample_unref(v0b);
  if (v1b) gst_sample_unref(v1b);
  if (pkb) gst_sample_unref(pkb);
  CHECK(get_u64(p.gate, "admitted-batches") == 2);
  destroy_pipe(&p);
}

static void test_push_fail_no_forged_credit() {
  MiniPipe p = make_pipe(false, true, true);
  if (!p.gate) return;
  push_full(&p, 0);
  g_usleep(200 * 1000);
  CHECK(get_u(p.gate, "credit") == 0);
  CHECK(emit_complete(p.gate, 0x3, TRUE) == FALSE);
  destroy_pipe(&p);
}

static void test_flush_releases_pending() {
  MiniPipe p = make_pipe();
  if (!p.gate) return;
  push_full(&p, 0);
  GstSample *v0 = pull(p.asink_v0, 800);
  GstSample *v1 = pull(p.asink_v1, 800);
  GstSample *pk = pull(p.asink_packed, 800);
  if (v0) gst_sample_unref(v0);
  if (v1) gst_sample_unref(v1);
  if (pk) gst_sample_unref(pk);
  push_full(&p, 40 * GST_MSECOND, 2);
  g_usleep(50 * 1000);
  GstPad *sink = gst_element_get_static_pad(p.gate, "sink");
  CHECK(sink != nullptr);
  if (sink) {
    gst_pad_send_event(sink, gst_event_new_flush_start());
    gst_pad_send_event(sink, gst_event_new_flush_stop(TRUE));
    gst_object_unref(sink);
  }
  g_usleep(50 * 1000);
  CHECK(get_u(p.gate, "pending-full-batch") == 0);
  CHECK(get_u(p.gate, "credit") == 1);
  destroy_pipe(&p);
}

static void test_sticky_order() {
  MiniPipe p = make_pipe();
  if (!p.gate) return;
  std::vector<GstEventType> evs;
  GstPad *v0 = gst_element_get_static_pad(p.gate, "video_0");
  CHECK(v0 != nullptr);
  gst_pad_add_probe(
      v0, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      [](GstPad *, GstPadProbeInfo *info, gpointer user_data) {
        auto *out = static_cast<std::vector<GstEventType> *>(user_data);
        GstEvent *e = GST_PAD_PROBE_INFO_EVENT(info);
        if (e) out->push_back(GST_EVENT_TYPE(e));
        return GST_PAD_PROBE_OK;
      },
      &evs, nullptr);
  gst_object_unref(v0);
  push_full(&p, 0);
  GstSample *s = pull(p.asink_v0, 800);
  if (s) gst_sample_unref(s);
  pull(p.asink_v1, 800);
  pull(p.asink_packed, 800);
  bool saw_ss = false, saw_caps = false, saw_seg = false;
  int order = 0;
  for (GstEventType t : evs) {
    if (t == GST_EVENT_STREAM_START && !saw_ss) {
      saw_ss = true;
      CHECK(order == 0);
      order = 1;
    } else if (t == GST_EVENT_CAPS && saw_ss && !saw_caps) {
      saw_caps = true;
      CHECK(order == 1);
      order = 2;
    } else if (t == GST_EVENT_SEGMENT && saw_caps && !saw_seg) {
      saw_seg = true;
      CHECK(order == 2);
    }
  }
  CHECK(saw_ss && saw_caps && saw_seg);
  destroy_pipe(&p);
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  if (!gst_element_factory_find("aiboxbatchgate")) {
    fprintf(stderr, "aiboxbatchgate factory missing; set GST_PLUGIN_PATH\n");
    return 2;
  }
  GstPlugin *fm = gst_plugin_load_by_name("aiboxframemeta");
  if (!fm) {
    fprintf(stderr, "aiboxframemeta missing; FrameMeta tests need it\n");
    return 2;
  }
  gst_object_unref(fm);
  test_names();
  test_admit_credit_and_caps();
  test_partial_no_credit();
  test_invalid_and_overload();
  test_push_fail_no_forged_credit();
  test_flush_releases_pending();
  test_sticky_order();
  if (g_failures) {
    fprintf(stderr, "test_aiboxbatchgate FAIL count=%d\n", g_failures);
    return 1;
  }
  fprintf(stdout, "test_aiboxbatchgate PASS\n");
  return 0;
}
