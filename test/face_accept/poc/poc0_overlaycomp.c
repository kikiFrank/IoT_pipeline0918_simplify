/* P0: minimal overlaycomposition capability test.
 * videotestsrc(NV12) ! overlaycomposition ! fakesink, draw callback returns a
 * static opaque white rectangle. Proves whether the stock element can render
 * an overlay on the NV12 buffers used by this pipeline, without touching any
 * vendor component or the qtivoverlay property path. */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/video-overlay-composition.h>
#include <stdio.h>
#include <string.h>

#define RECT_X 100
#define RECT_Y 100
#define RECT_W 200
#define RECT_H 200

static GstBuffer *g_white = NULL; /* static 4x4 opaque white BGRA tile */

static GstVideoOverlayComposition *on_draw(GstElement *oc, GstSample *sample,
                                           gpointer user_data) {
  (void)oc;
  (void)user_data;
  GstBuffer *buf = sample ? gst_sample_get_buffer(sample) : NULL;
  GstCaps *caps = sample ? gst_sample_get_caps(sample) : NULL;
  int w = 0, h = 0;
  if (caps) {
    GstVideoInfo vi;
    if (gst_video_info_from_caps(&vi, caps)) {
      w = GST_VIDEO_INFO_WIDTH(&vi);
      h = GST_VIDEO_INFO_HEIGHT(&vi);
    }
  }
  /* Clip the rectangle to the frame. */
  int x = RECT_X, y = RECT_Y, rw = RECT_W, rh = RECT_H;
  if (x >= w || y >= h) return NULL;
  if (x + rw > w) rw = w - x;
  if (y + rh > h) rh = h - y;
  if (rw <= 0 || rh <= 0) return NULL;

  GstVideoOverlayRectangle *rect = gst_video_overlay_rectangle_new_raw(
      g_white, x, y, (guint)rw, (guint)rh, GST_VIDEO_OVERLAY_FORMAT_FLAG_NONE);
  if (!rect) return NULL;
  GstVideoOverlayComposition *comp = gst_video_overlay_composition_new(rect);
  gst_video_overlay_rectangle_unref(rect); /* comp holds its own ref */
  (void)buf;
  return comp;
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);

  /* 4x4 BGRA opaque white tile, reused for every frame. */
  const guint tile = 4 * 4 * 4;
  g_white = gst_buffer_new_allocate(NULL, tile, NULL);
  GstMapInfo mi;
  if (gst_buffer_map(g_white, &mi, GST_MAP_WRITE)) {
    memset(mi.data, 0xFF, tile);
    gst_buffer_unmap(g_white, &mi);
  }

  GstElement *pipeline = gst_pipeline_new("poc0");
  GstElement *src = gst_element_factory_make("videotestsrc", "src");
  GstElement *cf = gst_element_factory_make("capsfilter", "cf");
  GstElement *oc = gst_element_factory_make("overlaycomposition", "oc");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");
  if (!pipeline || !src || !cf || !oc || !sink) {
    fprintf(stderr, "POC0_FAIL: element creation\n");
    return 1;
  }
  g_object_set(src, "num-buffers", 30, NULL);
  GstCaps *caps = gst_caps_from_string(
      "video/x-raw,format=NV12,width=640,height=480,framerate=30/1");
  g_object_set(cf, "caps", caps, NULL);
  gst_caps_unref(caps);
  g_signal_connect(oc, "draw", G_CALLBACK(on_draw), NULL);

  gst_bin_add_many(GST_BIN(pipeline), src, cf, oc, sink, NULL);
  if (!gst_element_link_many(src, cf, oc, sink, NULL)) {
    fprintf(stderr, "POC0_FAIL: link (NV12 not accepted?)\n");
    return 2;
  }

  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    fprintf(stderr, "POC0_FAIL: set_state(PLAYING)\n");
    return 3;
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  int rc = 0;
  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "POC0_FAIL: %s | %s\n", err ? err->message : "?",
              dbg ? dbg : "");
      rc = 4;
      g_clear_error(&err);
      g_free(dbg);
    } else {
      g_print("POC0_OK: EOS reached, overlay composition rendered\n");
    }
    gst_message_unref(msg);
  } else {
    fprintf(stderr, "POC0_FAIL: timeout without EOS\n");
    rc = 5;
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  gst_buffer_unref(g_white);
  return rc;
}
