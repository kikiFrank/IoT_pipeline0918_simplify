#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>

#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/MediaReadyEvent.hpp"

namespace {

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

#define CHECK_EQ(a, b) CHECK_TRUE((a) == (b))

std::string MakeTempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/aibox_media_%s_XXXXXX", tag);
  char* created = mkdtemp(tmpl);
  if (!created) return {};
  return std::string(created);
}

void RemoveTree(const std::string& dir) {
  if (dir.empty() || dir.find("/tmp/aibox_media_") != 0) return;
  const std::string cmd = "rm -rf \"" + dir + "\"";
  (void)std::system(cmd.c_str());
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool TestWriteAbsolutePathAndContent() {
  const std::string dir = MakeTempDir("ok");
  CHECK_TRUE(!dir.empty());
  aibox::media::MediaFileWriter writer(dir);
  const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0x01, 0x02, 0xFF, 0xD9};
  const auto wr = writer.WriteJpeg("550e8400-e29b-41d4-a716-446655440000", jpeg);
  CHECK_TRUE(wr.ok);
  CHECK_TRUE(!wr.absolute_path.empty());
  CHECK_TRUE(wr.absolute_path[0] == '/');
  CHECK_EQ(wr.size_bytes, static_cast<int64_t>(jpeg.size()));
  CHECK_TRUE(FileExists(wr.absolute_path));

  std::ifstream in(wr.absolute_path, std::ios::binary);
  std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK_TRUE(got == jpeg);
  CHECK_TRUE(!FileExists(wr.absolute_path + ".tmp"));
  RemoveTree(dir);
  return true;
}

bool TestUnwritableDirFailsAndLeavesNoFile() {
  const std::string dir = MakeTempDir("ro");
  CHECK_TRUE(!dir.empty());
  CHECK_EQ(chmod(dir.c_str(), 0555), 0);

  aibox::media::MediaFileWriter writer(dir);
  const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
  const auto wr = writer.WriteJpeg("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", jpeg);
  CHECK_TRUE(!wr.ok);
  CHECK_TRUE(wr.absolute_path.empty());

  const std::string jpg = dir + "/aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee.jpg";
  CHECK_TRUE(!FileExists(jpg));
  CHECK_TRUE(!FileExists(jpg + ".tmp"));

  (void)chmod(dir.c_str(), 0755);
  RemoveTree(dir);
  return true;
}

bool TestRejectsUnsafeEventId() {
  const std::string dir = MakeTempDir("bad");
  CHECK_TRUE(!dir.empty());
  aibox::media::MediaFileWriter writer(dir);
  const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
  const auto wr = writer.WriteJpeg("../escape", jpeg);
  CHECK_TRUE(!wr.ok);
  RemoveTree(dir);
  return true;
}

bool TestWriteThumbnail() {
  const std::string dir = MakeTempDir("thumb");
  CHECK_TRUE(!dir.empty());
  aibox::media::MediaFileWriter writer(dir);
  const std::string id = "550e8400-e29b-41d4-a716-446655440011";
  const std::vector<uint8_t> thumb = {0xFF, 0xD8, 0x02, 0x03, 0xFF, 0xD9};
  const auto wr = writer.WriteThumbnail(id, thumb);
  CHECK_TRUE(wr.ok);
  CHECK_TRUE(!wr.absolute_path.empty());
  CHECK_TRUE(wr.absolute_path.find(id + ".thumb.jpg") != std::string::npos);
  CHECK_EQ(wr.size_bytes, static_cast<int64_t>(thumb.size()));
  CHECK_TRUE(FileExists(wr.absolute_path));
  CHECK_TRUE(!FileExists(wr.absolute_path + ".tmp"));

  // Original .jpg and thumbnail coexist under distinct names.
  const auto jpg = writer.WriteJpeg(id, thumb);
  CHECK_TRUE(jpg.ok);
  CHECK_TRUE(FileExists(jpg.absolute_path));
  CHECK_TRUE(jpg.absolute_path.find(id + ".jpg") != std::string::npos);
  RemoveTree(dir);
  return true;
}

bool TestWriteThumbnailUnwritableDir() {
  const std::string dir = MakeTempDir("thumro");
  CHECK_TRUE(!dir.empty());
  CHECK_EQ(chmod(dir.c_str(), 0555), 0);

  aibox::media::MediaFileWriter writer(dir);
  const std::vector<uint8_t> thumb = {0xFF, 0xD8, 0xFF, 0xD9};
  const std::string id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  const auto wr = writer.WriteThumbnail(id, thumb);
  CHECK_TRUE(!wr.ok);
  CHECK_TRUE(wr.absolute_path.empty());
  const std::string path = dir + "/" + id + ".thumb.jpg";
  CHECK_TRUE(!FileExists(path));
  CHECK_TRUE(!FileExists(path + ".tmp"));

  (void)chmod(dir.c_str(), 0755);
  RemoveTree(dir);
  return true;
}

bool TestMediaReadyJsonFieldsAndEscape() {
  aibox::media::MediaReadyEvent ev;
  ev.event_id = "550e8400-e29b-41d4-a716-446655440000";
  ev.file_path = "/tmp/aibox/\"quote\".jpg";
  ev.thumbnail_path = "/tmp/aibox/\"quote\".thumb.jpg";
  ev.size_bytes = 12;
  ev.camera_id = "cam-001";
  ev.timestamp_ms = 1750000000000LL;
  const std::string json = ev.ToJson();
  CHECK_TRUE(json.front() == '{');
  CHECK_TRUE(json.find("\"event_id\":\"550e8400-e29b-41d4-a716-446655440000\"") !=
             std::string::npos);
  CHECK_TRUE(json.find("\"file_path\":\"/tmp/aibox/\\\"quote\\\".jpg\"") !=
             std::string::npos);
  CHECK_TRUE(json.find("\"size_bytes\":12") != std::string::npos);
  CHECK_TRUE(json.find("\"thumbnail_path\":\"/tmp/aibox/\\\"quote\\\".thumb.jpg\"") !=
             std::string::npos);
  CHECK_TRUE(json.find("\"camera_id\":\"cam-001\"") != std::string::npos);
  CHECK_TRUE(json.find("\"timestamp_ms\":1750000000000") != std::string::npos);

  // Key is always emitted; empty thumbnail_path serializes as "".
  aibox::media::MediaReadyEvent empty_thumb;
  empty_thumb.event_id = "e";
  empty_thumb.file_path = "f";
  const std::string json2 = empty_thumb.ToJson();
  CHECK_TRUE(json2.find("\"thumbnail_path\":\"\"") != std::string::npos);
  return true;
}

}  // namespace

int main() {
  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"WriteAbsolutePathAndContent", TestWriteAbsolutePathAndContent},
      {"UnwritableDirFailsAndLeavesNoFile", TestUnwritableDirFailsAndLeavesNoFile},
      {"RejectsUnsafeEventId", TestRejectsUnsafeEventId},
      {"WriteThumbnail", TestWriteThumbnail},
      {"WriteThumbnailUnwritableDir", TestWriteThumbnailUnwritableDir},
      {"MediaReadyJsonFieldsAndEscape", TestMediaReadyJsonFieldsAndEscape},
  };

  for (const auto& test : tests) {
    if (!test.fn()) {
      std::cerr << "FAILED " << test.name << '\n';
      return 1;
    }
    std::cout << "PASSED " << test.name << '\n';
  }
  return 0;
}
