#pragma once

#include <cstdint>
#include <vector>

typedef struct _GstElement GstElement;

namespace aibox::media {

// Thumbnail encoding: CPU packed NV12 in, 320x240 JPEG (quality 85) out.
// Same contract style as ISnapshotEncoder; the service thread owns one
// instance and uses it serially (same-thread safe, like GstSnapshotEncoder).
class IThumbnailEncoder {
 public:
  static constexpr uint32_t kThumbWidth = 320;
  static constexpr uint32_t kThumbHeight = 240;

  virtual ~IThumbnailEncoder() = default;
  virtual bool EncodeNv12(const uint8_t* packed_nv12, uint32_t width,
                          uint32_t height,
                          std::vector<uint8_t>& thumb_jpeg_out) = 0;
};

// Persistent appsrc → videoscale (fixed 320x240) → videoconvert → jpegenc
// (quality 85) → appsink. Input is CPU packed NV12 (not DMA-BUF).
// A failed Build leaves IsReady() == false and every EncodeNv12 call
// returning false — callers degrade gracefully (thumbnail_path stays empty).
class GstThumbnailEncoder final : public IThumbnailEncoder {
 public:
  GstThumbnailEncoder();
  ~GstThumbnailEncoder() override;

  GstThumbnailEncoder(const GstThumbnailEncoder&) = delete;
  GstThumbnailEncoder& operator=(const GstThumbnailEncoder&) = delete;

  // True when jpegenc + videoscale + videoconvert element factories exist.
  static bool Available();
  bool IsReady() const;

  bool EncodeNv12(const uint8_t* packed_nv12, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& thumb_jpeg_out) override;

 private:
  bool Build();
  void Teardown();
  bool EnsureCaps(uint32_t width, uint32_t height);

  GstElement* pipeline_ = nullptr;
  GstElement* appsrc_ = nullptr;
  GstElement* appsink_ = nullptr;
  uint32_t caps_width_ = 0;
  uint32_t caps_height_ = 0;
};

}  // namespace aibox::media
