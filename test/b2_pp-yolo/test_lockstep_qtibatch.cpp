// Phase 4-R1: lockstep collect+push → qtibatch emits only 0x3.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <vector>

#include "aibox/pipeline/BatchLockstep.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

struct Seen {
  std::vector<guint> masks;
};

static GstPadProbeReturn
on_src(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* seen = static_cast<Seen*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  const guint64 raw = GST_BUFFER_OFFSET(buf);
  const guint mask = (raw == GST_BUFFER_OFFSET_NONE)
                         ? 0xFFu
                         : static_cast<guint>(raw & 0xFFu);
  seen->masks.push_back(mask);
  fprintf(stderr, "[LS_SRC] n=%zu mask=0x%x pts=%" GST_TIME_FORMAT "\n",
          seen->masks.size(), mask, GST_TIME_ARGS(GST_BUFFER_PTS(buf)));
  return GST_PAD_PROBE_OK;
}

static int count_mask(const Seen& s, guint m) {
  int n = 0;
  for (guint x : s.masks)
    if (x == m) ++n;
  return n;
}

static GstBuffer* make_nv12(GstClockTime pts) {
  const gsize sz = 320 * 240 + 320 * 120;
  GstBuffer* b = gst_buffer_new_allocate(nullptr, sz, nullptr);
  GST_BUFFER_PTS(b) = pts;
  GST_BUFFER_DTS(b) = pts;
  GST_BUFFER_DURATION(b) = 40 * GST_MSECOND;
  return b;
}

static void dump_bus(GstElement* pipe) {
  GstBus* bus = gst_element_get_bus(pipe);
  while (true) {
    GstMessage* msg = gst_bus_pop(bus);
    if (!msg) break;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "[LS_BUS] ERROR %s %s\n", err ? err->message : "",
              dbg ? dbg : "");
      if (err) g_error_free(err);
      g_free(dbg);
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
}

static void configure_appsrc(GstElement* src) {
  GstCaps* caps = gst_caps_from_string(
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1");
  g_object_set(src, "is-live", FALSE, "format", GST_FORMAT_TIME,
               "block", FALSE, "max-bytes", (guint64)0, nullptr);
  gst_app_src_set_caps(GST_APP_SRC(src), caps);
  gst_caps_unref(caps);
}

static int test_appsrc_same_tick_after_gap() {
  fprintf(stderr, "== lockstep case1 appsrc same-tick after 150ms gap ==\n");
  Seen seen;
  GstElement* pipe = gst_parse_launch(
      "appsrc name=a0 ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "qtibatch name=b moving-window-size=1 sink_0. "
      "appsrc name=a1 ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "b.sink_1 b. ! fakesink sync=false async=false",
      nullptr);
  CHECK(pipe != nullptr);
  if (!pipe) return 1;
  GstElement* batch = gst_bin_get_by_name(GST_BIN(pipe), "b");
  GstElement* a0 = gst_bin_get_by_name(GST_BIN(pipe), "a0");
  GstElement* a1 = gst_bin_get_by_name(GST_BIN(pipe), "a1");
  configure_appsrc(a0);
  configure_appsrc(a1);
  GstPad* src = batch ? gst_element_get_static_pad(batch, "src") : nullptr;
  if (src) {
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, on_src, &seen, nullptr);
    gst_object_unref(src);
  }
  gst_element_set_state(pipe, GST_STATE_PLAYING);
  GstState st = GST_STATE_VOID_PENDING;
  gst_element_get_state(pipe, &st, nullptr, 2 * GST_SECOND);
  fprintf(stderr, "LS_CASE1 state=%s\n", gst_element_state_get_name(st));

  for (int i = 0; i < 8; ++i) {
    const GstClockTime pts = static_cast<GstClockTime>(i) * 40 * GST_MSECOND;
    gst_app_src_push_buffer(GST_APP_SRC(a0), make_nv12(pts));
    gst_app_src_push_buffer(GST_APP_SRC(a1), make_nv12(pts));
    g_usleep(150 * 1000);
  }
  g_usleep(200 * 1000);
  dump_bus(pipe);
  fprintf(stderr, "LS_CASE1 full=0x3 count=%d partial1=%d partial2=%d total=%zu\n",
          count_mask(seen, 0x3), count_mask(seen, 0x1), count_mask(seen, 0x2),
          seen.masks.size());
  CHECK(count_mask(seen, 0x3) >= 6);
  CHECK(count_mask(seen, 0x1) == 0);
  CHECK(count_mask(seen, 0x2) == 0);
  gst_element_set_state(pipe, GST_STATE_NULL);
  if (a0) gst_object_unref(a0);
  if (a1) gst_object_unref(a1);
  if (batch) gst_object_unref(batch);
  gst_object_unref(pipe);
  return 0;
}

static int test_collect_push_with_skew() {
  fprintf(stderr, "== lockstep case2 collect+push across 80ms skew ==\n");
  Seen seen;
  GstElement* pipe = gst_parse_launch(
      "videotestsrc name=v0 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "identity name=i0 ! fakesink name=d0 async=false sync=false "
      "videotestsrc name=v1 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "identity name=i1 sleep-time=80000 ! fakesink name=d1 async=false sync=false "
      "qtibatch name=b moving-window-size=1 ! fakesink name=out async=false sync=false",
      nullptr);
  CHECK(pipe != nullptr);
  if (!pipe) return 1;

  GstElement* batch = gst_bin_get_by_name(GST_BIN(pipe), "b");
  GstElement* i0 = gst_bin_get_by_name(GST_BIN(pipe), "i0");
  GstElement* i1 = gst_bin_get_by_name(GST_BIN(pipe), "i1");
  GstPad* bsrc = batch ? gst_element_get_static_pad(batch, "src") : nullptr;
  GstPad* p0 = i0 ? gst_element_get_static_pad(i0, "src") : nullptr;
  GstPad* p1 = i1 ? gst_element_get_static_pad(i1, "src") : nullptr;
  GstPad* s0 = batch ? gst_element_request_pad_simple(batch, "sink_0") : nullptr;
  GstPad* s1 = batch ? gst_element_request_pad_simple(batch, "sink_1") : nullptr;
  if (bsrc) {
    gst_pad_add_probe(bsrc, GST_PAD_PROBE_TYPE_BUFFER, on_src, &seen, nullptr);
    gst_object_unref(bsrc);
  }

  BatchLockstepBarrier bar;
  batch_lockstep_init(&bar);
  batch_lockstep_set_dest(&bar, 0, s0);
  batch_lockstep_set_dest(&bar, 1, s1);
  auto* c0 = new BatchLockstepSlot{&bar, 0, "i0"};
  auto* c1 = new BatchLockstepSlot{&bar, 1, "i1"};
  const GstPadProbeType mask = static_cast<GstPadProbeType>(
      GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM);
  if (p0)
    gst_pad_add_probe(p0, mask, OnBatchLockstepCollectProbe, c0, nullptr);
  if (p1)
    gst_pad_add_probe(p1, mask, OnBatchLockstepCollectProbe, c1, nullptr);

  gst_element_set_state(pipe, GST_STATE_PLAYING);
  gst_element_get_state(pipe, nullptr, nullptr, 2 * GST_SECOND);
  g_usleep(1500 * 1000);
  dump_bus(pipe);
  fprintf(stderr,
          "LS_CASE2 full=0x3 count=%d partial1=%d partial2=%d total=%zu pairs=%lu\n",
          count_mask(seen, 0x3), count_mask(seen, 0x1), count_mask(seen, 0x2),
          seen.masks.size(), static_cast<unsigned long>(bar.pairs.load()));
  CHECK(count_mask(seen, 0x3) >= 4);
  CHECK(count_mask(seen, 0x1) == 0);
  CHECK(count_mask(seen, 0x2) == 0);

  batch_lockstep_shutdown(&bar);
  gst_element_set_state(pipe, GST_STATE_NULL);
  gst_element_get_state(pipe, nullptr, nullptr, GST_SECOND);
  delete c0;
  delete c1;
  batch_lockstep_clear(&bar);
  if (s0 && batch) gst_element_release_request_pad(batch, s0);
  if (s1 && batch) gst_element_release_request_pad(batch, s1);
  if (s0) gst_object_unref(s0);
  if (s1) gst_object_unref(s1);
  if (p0) gst_object_unref(p0);
  if (p1) gst_object_unref(p1);
  if (i0) gst_object_unref(i0);
  if (i1) gst_object_unref(i1);
  if (batch) gst_object_unref(batch);
  gst_object_unref(pipe);
  return 0;
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!gst_element_factory_find("qtibatch")) {
    fprintf(stderr, "qtibatch factory missing\n");
    return 2;
  }
  test_collect_push_with_skew();
  if (g_failures) {
    fprintf(stderr, "test_lockstep_qtibatch FAIL count=%d\n", g_failures);
    return 1;
  }
  fprintf(stdout, "test_lockstep_qtibatch PASS\n");
  return 0;
}
