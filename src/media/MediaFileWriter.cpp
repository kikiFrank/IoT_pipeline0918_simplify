#include "aibox/media/MediaFileWriter.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace aibox::media {
namespace {

bool IsSafeEventId(const std::string& event_id) {
  if (event_id.empty()) return false;
  for (unsigned char c : event_id) {
    if (c == '/' || c == '\\' || c == '\0' || c < 0x20) return false;
  }
  return true;
}

bool EnsureDirRecursive(const std::string& dir) {
  if (dir.empty()) return false;
  struct stat st {};
  if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

  const auto slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    if (!EnsureDirRecursive(dir.substr(0, slash))) return false;
  }
  if (mkdir(dir.c_str(), 0755) == 0) return true;
  if (errno == EEXIST && stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  return false;
}

std::string AbsolutePath(const std::string& path) {
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved) != nullptr) return std::string(resolved);
  if (!path.empty() && path[0] == '/') return path;
  return {};
}

}  // namespace

MediaFileWriter::MediaFileWriter(std::string media_dir)
    : media_dir_(std::move(media_dir)) {}

bool MediaFileWriter::EnsureDirectory() const {
  return EnsureDirRecursive(media_dir_);
}

MediaWriteResult MediaFileWriter::WriteJpeg(const std::string& event_id,
                                            const std::vector<uint8_t>& data) const {
  return WriteJpeg(event_id, data.data(), data.size());
}

MediaWriteResult MediaFileWriter::WriteJpeg(const std::string& event_id,
                                            const uint8_t* data,
                                            size_t size) const {
  if (!IsSafeEventId(event_id)) return {};
  return WriteData(media_dir_ + "/" + event_id + ".jpg", data, size);
}

MediaWriteResult MediaFileWriter::WriteThumbnail(
    const std::string& event_id, const std::vector<uint8_t>& data) const {
  return WriteThumbnail(event_id, data.data(), data.size());
}

MediaWriteResult MediaFileWriter::WriteThumbnail(const std::string& event_id,
                                                 const uint8_t* data,
                                                 size_t size) const {
  if (!IsSafeEventId(event_id)) return {};
  return WriteData(media_dir_ + "/" + event_id + ".thumb.jpg", data, size);
}

MediaWriteResult MediaFileWriter::WriteData(const std::string& final_path,
                                            const uint8_t* data,
                                            size_t size) const {
  MediaWriteResult result;
  if (!data || size == 0) return result;
  if (!EnsureDirectory()) return result;

  const std::string tmp_path = final_path + ".tmp";

  FILE* fp = std::fopen(tmp_path.c_str(), "wb");
  if (!fp) return result;
  const size_t written = std::fwrite(data, 1, size, fp);
  const int flush_rc = std::fflush(fp);
  std::fclose(fp);
  if (written != size || flush_rc != 0) {
    std::remove(tmp_path.c_str());
    return result;
  }
  if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    return result;
  }

  result.absolute_path = AbsolutePath(final_path);
  if (result.absolute_path.empty()) {
    std::remove(final_path.c_str());
    return result;
  }
  result.size_bytes = static_cast<int64_t>(size);
  result.ok = true;
  return result;
}

}  // namespace aibox::media
