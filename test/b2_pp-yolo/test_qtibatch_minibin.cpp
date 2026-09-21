// Phase 3-R1: controllable sources → qtibatch → fakesink. No real QNN.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <vector>

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
  fprintf(stderr, "[R1_SRC] n=%zu mask=0x%x pts=%" GST_TIME_FORMAT " size=%zu\n",
          seen->masks.size(), mask, GST_TIME_ARGS(GST_BUFFER_PTS(buf)),
          gst_buffer_get_size(buf));
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
drop_block(GstPad*, GstPadProbeInfo*, gpointer) {
  return GST_PAD_PROBE_OK;
}

static int count_mask(const Seen& s, guint m) {
  int n = 0;
  for (guint x : s.masks)
    if (x == m) ++n;
  return n;
}

static GstCaps* raw_caps() {
  return gst_caps_from_string(
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1");
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
      fprintf(stderr, "[R1_BUS] ERROR %s %s\n", err ? err->message : "",
              dbg ? dbg : "");
      if (err) g_error_free(err);
      g_free(dbg);
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
}

static int test_same_rate() {
  fprintf(stderr, "== R1 case1 videotestsrc same-rate ==\n");
  Seen seen;
  GstElement* pipe = gst_parse_launch(
      "videotestsrc name=v0 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "qtibatch name=b moving-window-size=1 sink_0. "
      "videotestsrc name=v1 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "b.sink_1 b. ! fakesink sync=false async=false",
      nullptr);
  CHECK(pipe != nullptr);
  if (!pipe) return 1;
  GstElement* batch = gst_bin_get_by_name(GST_BIN(pipe), "b");
  GstPad* src = batch ? gst_element_get_static_pad(batch, "src") : nullptr;
  if (src) {
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, on_src, &seen, nullptr);
    gst_object_unref(src);
  }
  if (batch) gst_object_unref(batch);
  gst_element_set_state(pipe, GST_STATE_PLAYING);
  gst_element_get_state(pipe, nullptr, nullptr, 2 * GST_SECOND);
  g_usleep(1500 * 1000);
  dump_bus(pipe);
  gst_element_set_state(pipe, GST_STATE_NULL);
  gst_object_unref(pipe);
  fprintf(stderr, "R1_CASE1 full=0x3 count=%d total=%zu\n", count_mask(seen, 0x3),
          seen.masks.size());
  CHECK(count_mask(seen, 0x3) >= 4);
  return 0;
}

static GstBuffer* make_nv12(GstClockTime pts) {
  const gsize sz = 320 * 240 + 320 * 120;
  GstBuffer* b = gst_buffer_new_allocate(nullptr, sz, nullptr);
  GST_BUFFER_PTS(b) = pts;
  GST_BUFFER_DTS(b) = pts;
  GST_BUFFER_DURATION(b) = 40 * GST_MSECOND;
  return b;
}

static int test_overdue_padblock() {
  fprintf(stderr, "== R1 case2 pad-block overdue then sink_0 then sink_1 ==\n");
  Seen seen;
  GstElement* pipe = gst_parse_launch(
      "videotestsrc name=v0 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "identity name=i0 ! qtibatch name=b moving-window-size=1 sink_0. "
      "videotestsrc name=v1 is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1 ! "
      "identity name=i1 ! b.sink_1 b. ! fakesink sync=false async=false",
      nullptr);
  CHECK(pipe != nullptr);
  if (!pipe) return 1;
  GstElement* batch = gst_bin_get_by_name(GST_BIN(pipe), "b");
  GstElement* i0 = gst_bin_get_by_name(GST_BIN(pipe), "i0");
  GstElement* i1 = gst_bin_get_by_name(GST_BIN(pipe), "i1");
  GstPad* bsrc = batch ? gst_element_get_static_pad(batch, "src") : nullptr;
  if (bsrc) {
    gst_pad_add_probe(bsrc, GST_PAD_PROBE_TYPE_BUFFER, on_src, &seen, nullptr);
    gst_object_unref(bsrc);
  }
  GstPad* p0 = i0 ? gst_element_get_static_pad(i0, "src") : nullptr;
  GstPad* p1 = i1 ? gst_element_get_static_pad(i1, "src") : nullptr;
  gst_element_set_state(pipe, GST_STATE_PLAYING);
  gst_element_get_state(pipe, nullptr, nullptr, 2 * GST_SECOND);
  g_usleep(250 * 1000);
  const size_t after_warm = seen.masks.size();
  fprintf(stderr, "R1_CASE2 warm total=%zu\n", after_warm);

  gulong id0 = gst_pad_add_probe(p0, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                 drop_block, nullptr, nullptr);
  gulong id1 = gst_pad_add_probe(p1, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                 drop_block, nullptr, nullptr);
  g_usleep(250 * 1000);
  gst_pad_remove_probe(p0, id0);
  g_usleep(80 * 1000);
  id0 = gst_pad_add_probe(p0, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, drop_block,
                          nullptr, nullptr);
  g_usleep(80 * 1000);
  gst_pad_remove_probe(p1, id1);
  g_usleep(200 * 1000);
  gst_pad_remove_probe(p0, id0);

  bool saw_1 = false, saw_2 = false;
  for (size_t i = after_warm; i < seen.masks.size(); ++i) {
    if (seen.masks[i] == 0x1) saw_1 = true;
    if (seen.masks[i] == 0x2) saw_2 = true;
    fprintf(stderr, "R1_CASE2 post mask=0x%x\n", seen.masks[i]);
  }
  fprintf(stderr, "R1_CASE2 after_warm=%zu total=%zu saw_0x1=%d saw_0x2=%d\n",
          after_warm, seen.masks.size(), saw_1 ? 1 : 0, saw_2 ? 1 : 0);
  CHECK(saw_1);
  CHECK(saw_2);
  dump_bus(pipe);
  if (p0) gst_object_unref(p0);
  if (p1) gst_object_unref(p1);
  if (i0) gst_object_unref(i0);
  if (i1) gst_object_unref(i1);
  if (batch) gst_object_unref(batch);
  gst_element_set_state(pipe, GST_STATE_NULL);
  gst_element_get_state(pipe, nullptr, nullptr, GST_SECOND);
  gst_object_unref(pipe);
  return 0;
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!gst_element_factory_find("qtibatch")) {
    fprintf(stderr, "qtibatch factory missing\n");
    return 2;
  }
  test_same_rate();
  test_overdue_padblock();
  if (g_failures) {
    fprintf(stderr, "test_qtibatch_minibin FAIL count=%d\n", g_failures);
    return 1;
  }
  fprintf(stdout, "test_qtibatch_minibin PASS\n");
  return 0;
}
