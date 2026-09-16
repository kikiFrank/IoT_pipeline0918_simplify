/* P0b: overlaycomposition capability test + pixel verification.
 * Adds GstVideoMeta to the static white tile (required by
 * gst_video_overlay_rectangle_new_raw) and checks the composited NV12 frame
 * downstream to prove the rectangle actually landed on the picture. */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/video-overlay-composition.h>
#include <stdio.h>
#include <string.h>

#define RECT_X 100
#define RECT_Y 100
#define RECT_W 200
#define RECT_H 200
#define PROBE_INSIDE_X 150
#define PROBE_INSIDE_Y 150
#define PROBE_OUTSIDE_X 20
#define PROBE_OUTSIDE_Y 40

static GstBuffer *g_white = NULL;
static int g_checked = 0;

static GstVideoOverlayComposition *on_draw(GstElement *oc, GstSample *sample,
                                           gpointer user_data) {
  (void)oc; (void)user_data;
  GstCaps *caps = sample ? gst_sample_get_caps(sample) : NULL;
  int w = 0, h = 0;
  if (caps) {
    GstVideoInfo vi;
    if (gst_video_info_from_caps(&vi, caps)) {
      w = GST_VIDEO_INFO_WIDTH(&vi);
      h = GST_VIDEO_INFO_HEIGHT(&vi);
    }
  }
  int x = RECT_X, y = RECT_Y, rw = RECT_W, rh = RECT_H;
  if (w > 0 && h > 0) {
    if (x >= w || y >= h) return NULL;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
  }
  if (rw <= 0 || rh <= 0) return NULL;

  GstVideoOverlayRectangle *rect = gst_video_overlay_rectangle_new_raw(
      g_white, x, y, (guint)rw, (guint)rh, GST_VIDEO_OVERLAY_FORMAT_FLAG_NONE);
  if (!rect) return NULL;
  GstVideoOverlayComposition *comp = gst_video_overlay_composition_new(rect);
  gst_video_overlay_rectangle_unref(rect);
  return comp;
}

static GstPadProbeReturn on_frame(GstPad *pad, GstPadProbeInfo *info,
                                  gpointer user_data) {
  (void)pad; (void)user_data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  if (++g_checked != 10) return GST_PAD_PROBE_OK; /* check the 10th frame */

  GstVideoInfo vi;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstVideoFrame vf;
  if (!caps || !gst_video_info_from_caps(&vi, caps)) {
    if (caps) gst_caps_unref(caps);
    return GST_PAD_PROBE_OK;
  }
  if (!gst_video_frame_map(&vf, &vi, buf, GST_MAP_READ)) {
    gst_caps_unref(caps);
    fprintf(stderr, "P0B_FAIL: cannot map composited frame\n");
    return GST_PAD_PROBE_OK;
  }
  const guint8 *yp = GST_VIDEO_FRAME_PLANE_DATA(&vf, 0);
  const gint ys = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0);
  const guint8 inside = yp[PROBE_INSIDE_Y * ys + PROBE_INSIDE_X];
  const guint8 outside = yp[PROBE_OUTSIDE_Y * ys + PROBE_OUTSIDE_X];
  const guint8 p2 = yp[110 * ys + 110], p3 = yp[290 * ys + 290];
  const guint8 p4 = yp[50 * ys + 500], p5 = yp[440 * ys + 40];
  fprintf(stderr,
          "P0B_PIXELS inside=(%d,%d)Y=%u outside=(%d,%d)Y=%u format=%s %dx%d\n",
          PROBE_INSIDE_X, PROBE_INSIDE_Y, inside, PROBE_OUTSIDE_X,
          PROBE_OUTSIDE_Y, outside,
          GST_VIDEO_INFO_NAME(&vi), GST_VIDEO_INFO_WIDTH(&vi),
          GST_VIDEO_INFO_HEIGHT(&vi));
  if (inside > 200 && outside < 200)
    fprintf(stderr, "P0B_RESULT=PASS (white rect composited)\n");
  else if (inside == outside)
    fprintf(stderr, "P0B_RESULT=FAIL (no visible rectangle)\n");
  else
    fprintf(stderr, "P0B_RESULT=INCONCLUSIVE inside=%u outside=%u\n", inside,
            outside);
  gst_video_frame_unmap(&vf);
  gst_caps_unref(caps);
  return GST_PAD_PROBE_OK;
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  const guint tile = 4 * 4 * 4;
  g_white = gst_buffer_new_allocate(NULL, tile, NULL);
  /* new_raw() asserts that the pixels buffer carries a GstVideoMeta. */
  gst_buffer_add_video_meta(g_white, GST_VIDEO_FRAME_FLAG_NONE,
                            GST_VIDEO_FORMAT_BGRA, 4, 4);
  GstMapInfo mi;
  if (gst_buffer_map(g_white, &mi, GST_MAP_WRITE)) {
    memset(mi.data, 0xFF, tile);
    gst_buffer_unmap(g_white, &mi);
  }

  GstElement *pipeline = gst_pipeline_new("poc0b");
  GstElement *src = gst_element_factory_make("videotestsrc", "src");
  GstElement *cf = gst_element_factory_make("capsfilter", "cf");
  GstElement *oc = gst_element_factory_make("overlaycomposition", "oc");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");
  if (!pipeline || !src || !cf || !oc || !sink) {
    fprintf(stderr, "P0B_FAIL: element creation\n");
    return 1;
  }
  g_object_set(src, "num-buffers", 30, NULL);
  g_object_set(src, "pattern", 2, NULL); /* 2 = black */
  GstCaps *caps = gst_caps_from_string(
      "video/x-raw,format=NV12,width=640,height=480,framerate=30/1");
  g_object_set(cf, "caps", caps, NULL);
  gst_caps_unref(caps);
  g_signal_connect(oc, "draw", G_CALLBACK(on_draw), NULL);

  gst_bin_add_many(GST_BIN(pipeline), src, cf, oc, sink, NULL);
  if (!gst_element_link_many(src, cf, oc, sink, NULL)) {
    fprintf(stderr, "P0B_FAIL: link\n");
    return 2;
  }
  GstPad *sp = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, on_frame, NULL, NULL);
  gst_object_unref(sp);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    fprintf(stderr, "P0B_FAIL: set_state\n");
    return 3;
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  int rc = 0;
  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err = NULL; gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "P0B_FAIL: %s | %s\n", err ? err->message : "?", dbg ? dbg : "");
      rc = 4;
      g_clear_error(&err); g_free(dbg);
    } else {
      fprintf(stderr, "P0B_OK: EOS\n");
    }
    gst_message_unref(msg);
  } else {
    fprintf(stderr, "P0B_FAIL: timeout\n");
    rc = 5;
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  gst_buffer_unref(g_white);
  return rc;
}
