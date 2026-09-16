#include "aibox/media/SnapshotEncoder.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

namespace aibox::media {
namespace {

constexpr GstClockTime kPullTimeout = 2 * GST_SECOND;

size_t PackedNv12Size(uint32_t width, uint32_t height) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
}

}  // namespace

bool GstSnapshotEncoder::JpegencAvailable() {
  GstElementFactory* factory = gst_element_factory_find("jpegenc");
  if (!factory) return false;
  gst_object_unref(factory);
  return true;
}

GstSnapshotEncoder::GstSnapshotEncoder() {
  Build();
}

GstSnapshotEncoder::~GstSnapshotEncoder() {
  Teardown();
}

bool GstSnapshotEncoder::IsReady() const {
  return pipeline_ != nullptr && appsrc_ != nullptr && appsink_ != nullptr;
}

bool GstSnapshotEncoder::Build() {
  if (!JpegencAvailable()) return false;

  GError* err = nullptr;
  pipeline_ = gst_parse_launch(
      "appsrc name=snap_src is-live=false format=time block=false max-bytes=0 "
      "! videoconvert name=snap_convert "
      "! jpegenc name=snap_enc quality=85 "
      "! appsink name=snap_sink sync=false max-buffers=1 drop=false",
      &err);
  if (!pipeline_) {
    if (err) g_error_free(err);
    return false;
  }
  if (err) g_error_free(err);

  appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "snap_src");
  appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "snap_sink");
  if (!appsrc_ || !appsink_) {
    Teardown();
    return false;
  }

  gst_app_src_set_stream_type(GST_APP_SRC(appsrc_), GST_APP_STREAM_TYPE_STREAM);
  g_object_set(appsrc_, "emit-signals", FALSE, nullptr);
  g_object_set(appsink_, "emit-signals", FALSE, "async", FALSE, nullptr);

  const GstStateChangeReturn ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    Teardown();
    return false;
  }
  GstState state = GST_STATE_VOID_PENDING;
  if (gst_element_get_state(pipeline_, &state, nullptr, kPullTimeout) !=
          GST_STATE_CHANGE_SUCCESS ||
      state != GST_STATE_PLAYING) {
    Teardown();
    return false;
  }
  return true;
}

void GstSnapshotEncoder::Teardown() {
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
  }
  if (appsrc_) {
    gst_object_unref(appsrc_);
    appsrc_ = nullptr;
  }
  if (appsink_) {
    gst_object_unref(appsink_);
    appsink_ = nullptr;
  }
  if (pipeline_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  caps_width_ = 0;
  caps_height_ = 0;
}

bool GstSnapshotEncoder::EnsureCaps(uint32_t width, uint32_t height) {
  if (caps_width_ == width && caps_height_ == height) return true;

  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, static_cast<int>(width),
      "height", G_TYPE_INT, static_cast<int>(height),
      "framerate", GST_TYPE_FRACTION, 1, 1,
      nullptr);
  if (!caps) return false;
  gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
  gst_caps_unref(caps);
  caps_width_ = width;
  caps_height_ = height;
  return true;
}

bool GstSnapshotEncoder::EncodeNv12(const uint8_t* packed_nv12, uint32_t width,
                                    uint32_t height,
                                    std::vector<uint8_t>& jpeg_out) {
  jpeg_out.clear();
  if (!IsReady() || !packed_nv12) return false;
  if (width < 2 || height < 2 || (width & 1u) || (height & 1u)) return false;

  const size_t nbytes = PackedNv12Size(width, height);
  if (!EnsureCaps(width, height)) return false;

  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, nbytes, nullptr);
  if (!buffer) return false;
  if (gst_buffer_fill(buffer, 0, packed_nv12, nbytes) != nbytes) {
    gst_buffer_unref(buffer);
    return false;
  }
  GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = GST_SECOND;

  const GstFlowReturn pushed =
      gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
  if (pushed != GST_FLOW_OK) return false;

  GstSample* sample =
      gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), kPullTimeout);
  if (!sample) return false;

  GstBuffer* out = gst_sample_get_buffer(sample);
  if (!out) {
    gst_sample_unref(sample);
    return false;
  }
  GstMapInfo map;
  if (!gst_buffer_map(out, &map, GST_MAP_READ) || map.size < 4) {
    gst_sample_unref(sample);
    return false;
  }
  jpeg_out.assign(map.data, map.data + map.size);
  gst_buffer_unmap(out, &map);
  gst_sample_unref(sample);
  return !jpeg_out.empty();
}

}  // namespace aibox::media
