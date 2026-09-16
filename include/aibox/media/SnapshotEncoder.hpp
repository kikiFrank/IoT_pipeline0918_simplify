#pragma once

#include <cstdint>
#include <vector>

typedef struct _GstElement GstElement;

namespace aibox::media {

class ISnapshotEncoder {
 public:
  virtual ~ISnapshotEncoder() = default;
  virtual bool EncodeNv12(const uint8_t* packed_nv12, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& jpeg_out) = 0;
};

// Persistent appsrc → videoconvert → jpegenc → appsink.
// Input is CPU packed NV12 (not DMA-BUF). Same-thread safe: appsrc
// block=FALSE, appsink try_pull_sample with timeout.
class GstSnapshotEncoder final : public ISnapshotEncoder {
 public:
  GstSnapshotEncoder();
  ~GstSnapshotEncoder() override;

  GstSnapshotEncoder(const GstSnapshotEncoder&) = delete;
  GstSnapshotEncoder& operator=(const GstSnapshotEncoder&) = delete;

  static bool JpegencAvailable();
  bool IsReady() const;

  bool EncodeNv12(const uint8_t* packed_nv12, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& jpeg_out) override;

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
