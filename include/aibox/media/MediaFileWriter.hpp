#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aibox::media {

struct MediaWriteResult {
  bool ok = false;
  std::string absolute_path;
  int64_t size_bytes = 0;
};

class MediaFileWriter {
 public:
  explicit MediaFileWriter(std::string media_dir);

  bool EnsureDirectory() const;
  MediaWriteResult WriteJpeg(const std::string& event_id, const uint8_t* data,
                             size_t size) const;
  MediaWriteResult WriteJpeg(const std::string& event_id,
                             const std::vector<uint8_t>& data) const;
  // Same atomic write semantics as WriteJpeg; file name <event_id>.thumb.jpg
  // (matches the backend ffmpeg thumbnail naming convention).
  MediaWriteResult WriteThumbnail(const std::string& event_id,
                                  const uint8_t* data, size_t size) const;
  MediaWriteResult WriteThumbnail(const std::string& event_id,
                                  const std::vector<uint8_t>& data) const;

  const std::string& Directory() const { return media_dir_; }

 private:
  MediaWriteResult WriteData(const std::string& final_path, const uint8_t* data,
                             size_t size) const;

  std::string media_dir_;
};

}  // namespace aibox::media
