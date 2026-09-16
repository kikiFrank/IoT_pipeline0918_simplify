#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "aibox/media/AnomalySnapshotService.hpp"
#include "aibox/media/IMediaReadyTransport.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/SnapshotEncoder.hpp"
#include "aibox/media/ThumbnailEncoder.hpp"

namespace {

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

#define CHECK_EQ(a, b) CHECK_TRUE((a) == (b))

class FakeEncoder final : public aibox::media::ISnapshotEncoder {
 public:
  bool succeed = true;
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& jpeg_out) override {
    jpeg_out.clear();
    if (!succeed) return false;
    jpeg_out = {0xFF, 0xD8, 0x00, 0x01, 0xFF, 0xD9};
    return true;
  }
};

class FakeThumbnailEncoder final : public aibox::media::IThumbnailEncoder {
 public:
  bool succeed = false;
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& thumb_out) override {
    thumb_out.clear();
    if (!succeed) return false;
    thumb_out = {0xFF, 0xD8, 0x00, 0x02, 0xFF, 0xD9};
    return true;
  }
};

class FakeTransport final : public aibox::media::IMediaReadyTransport {
 public:
  std::vector<std::string> payloads;
  bool PublishReady(const std::string& json) override {
    payloads.push_back(json);
    return true;
  }
};

std::string MakeTempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/aibox_media_%s_XXXXXX", tag);
  char* created = mkdtemp(tmpl);
  return created ? std::string(created) : std::string();
}

void RemoveTree(const std::string& dir) {
  if (dir.empty() || dir.find("/tmp/aibox_media_") != 0) return;
  const std::string cmd = "rm -rf \"" + dir + "\"";
  const int rc = std::system(cmd.c_str());
  (void)rc;
}

int CountJpg(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return -1;
  int n = 0;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 4 && name.substr(name.size() - 4) == ".jpg") ++n;
  }
  closedir(d);
  return n;
}

bool ExtractJsonString(const std::string& json, const std::string& key,
                       std::string& out) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  size_t i = pos + needle.size();
  out.clear();
  while (i < json.size() && json[i] != '"') {
    if (json[i] == '\\' && i + 1 < json.size()) {
      out.push_back(json[i + 1]);
      i += 2;
      continue;
    }
    out.push_back(json[i]);
    ++i;
  }
  return i < json.size();
}

aibox::media::SnapshotTask MakeTask(const std::string& rep,
                                    const std::vector<std::string>& ids) {
  aibox::media::SnapshotTask task;
  task.camera_id = "cam-001";
  task.timestamp_ms = 1750000000000LL;
  task.representative_event_id = rep;
  task.event_ids = ids;
  task.packed_nv12.assign(64 * 48 * 3 / 2, 80);
  task.width = 64;
  task.height = 48;
  return task;
}

bool TestOneFileThreeReady() {
  const std::string dir = MakeTempDir("svc3");
  CHECK_TRUE(!dir.empty());
  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  aibox::media::AnomalySnapshotService::SetProcessInstance(&service);
  CHECK_TRUE(aibox::media::AnomalySnapshotService::GetProcessInstance() ==
             &service);
  service.Start();

  const std::string rep = "550e8400-e29b-41d4-a716-4466554400aa";
  const std::vector<std::string> ids = {
      "550e8400-e29b-41d4-a716-4466554400aa",
      "550e8400-e29b-41d4-a716-4466554400bb",
      "550e8400-e29b-41d4-a716-4466554400cc",
  };
  CHECK_TRUE(service.Enqueue(MakeTask(rep, ids)));
  CHECK_TRUE(service.WaitUntilIdle(2000));
  CHECK_EQ(service.WrittenOk(), 1ULL);
  CHECK_EQ(service.Published(), 3ULL);
  CHECK_EQ(CountJpg(dir), 1);
  CHECK_EQ(transport.payloads.size(), static_cast<size_t>(3));

  std::string first_path;
  CHECK_TRUE(ExtractJsonString(transport.payloads[0], "file_path", first_path));
  CHECK_TRUE(first_path.find(rep + ".jpg") != std::string::npos);
  for (size_t i = 0; i < ids.size(); ++i) {
    std::string event_id;
    std::string file_path;
    CHECK_TRUE(ExtractJsonString(transport.payloads[i], "event_id", event_id));
    CHECK_TRUE(ExtractJsonString(transport.payloads[i], "file_path", file_path));
    CHECK_EQ(event_id, ids[i]);
    CHECK_EQ(file_path, first_path);
  }

  service.Stop();
  aibox::media::AnomalySnapshotService::SetProcessInstance(nullptr);
  CHECK_TRUE(aibox::media::AnomalySnapshotService::GetProcessInstance() ==
             nullptr);
  RemoveTree(dir);
  return true;
}

bool TestWriteFailurePublishesZero() {
  const std::string dir = MakeTempDir("svcfail");
  CHECK_TRUE(!dir.empty());
  CHECK_EQ(chmod(dir.c_str(), 0555), 0);

  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  aibox::media::AnomalySnapshotService::SetProcessInstance(&service);
  service.Start();
  CHECK_TRUE(service.Enqueue(MakeTask(
      "550e8400-e29b-41d4-a716-4466554400dd",
      {"550e8400-e29b-41d4-a716-4466554400dd"})));
  CHECK_TRUE(service.WaitUntilIdle(2000));
  CHECK_EQ(service.WrittenOk(), 0ULL);
  CHECK_EQ(service.WriteFailures(), 1ULL);
  CHECK_EQ(service.Published(), 0ULL);
  CHECK_TRUE(transport.payloads.empty());
  service.Stop();
  aibox::media::AnomalySnapshotService::SetProcessInstance(nullptr);
  (void)chmod(dir.c_str(), 0755);
  CHECK_EQ(CountJpg(dir), 0);
  RemoveTree(dir);
  return true;
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool TestThumbnailSuccess() {
  const std::string dir = MakeTempDir("svcthumb");
  CHECK_TRUE(!dir.empty());
  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;
  thumbnail_encoder.succeed = true;
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  service.Start();

  const std::string rep = "550e8400-e29b-41d4-a716-4466554400aa";
  const std::vector<std::string> ids = {
      "550e8400-e29b-41d4-a716-4466554400aa",
      "550e8400-e29b-41d4-a716-4466554400bb",
      "550e8400-e29b-41d4-a716-4466554400cc",
  };
  CHECK_TRUE(service.Enqueue(MakeTask(rep, ids)));
  CHECK_TRUE(service.WaitUntilIdle(2000));
  CHECK_EQ(service.WrittenOk(), 1ULL);
  CHECK_EQ(service.ThumbnailOk(), 1ULL);
  CHECK_EQ(service.Published(), 3ULL);
  CHECK_TRUE(FileExists(dir + "/" + rep + ".thumb.jpg"));

  std::string file_path;
  CHECK_TRUE(ExtractJsonString(transport.payloads[0], "file_path", file_path));
  for (size_t i = 0; i < ids.size(); ++i) {
    std::string thumbnail_path;
    CHECK_TRUE(ExtractJsonString(transport.payloads[i], "thumbnail_path",
                                 thumbnail_path));
    const std::string expected =
        file_path.substr(0, file_path.size() - 4) + ".thumb.jpg";
    CHECK_EQ(thumbnail_path, expected);
  }

  service.Stop();
  RemoveTree(dir);
  return true;
}

bool TestThumbnailFailureKeepsMessage() {
  const std::string dir = MakeTempDir("svcthumbfail");
  CHECK_TRUE(!dir.empty());
  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;  // succeed = false
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  service.Start();

  const std::string rep = "550e8400-e29b-41d4-a716-4466554400dd";
  CHECK_TRUE(service.Enqueue(MakeTask(rep, {rep})));
  CHECK_TRUE(service.WaitUntilIdle(2000));
  CHECK_EQ(service.WrittenOk(), 1ULL);
  CHECK_EQ(service.ThumbnailFailures(), 1ULL);
  CHECK_EQ(service.Published(), 1ULL);
  CHECK_TRUE(!FileExists(dir + "/" + rep + ".thumb.jpg"));

  std::string thumbnail_path;
  CHECK_TRUE(ExtractJsonString(transport.payloads[0], "thumbnail_path",
                               thumbnail_path));
  CHECK_EQ(thumbnail_path, std::string());

  service.Stop();
  RemoveTree(dir);
  return true;
}

bool TestDropOldestBeforeStart() {
  const std::string dir = MakeTempDir("svcdrop");
  CHECK_TRUE(!dir.empty());
  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  aibox::media::AnomalySnapshotService::SetProcessInstance(&service);

  for (int i = 0; i < 5; ++i) {
    const std::string id = "550e8400-e29b-41d4-a716-44665544000" +
                           std::to_string(i);
    CHECK_TRUE(service.Enqueue(MakeTask(id, {id})));
  }
  CHECK_EQ(service.Enqueued(), 5ULL);
  CHECK_EQ(service.Dropped(), 3ULL);

  service.Start();
  CHECK_TRUE(service.WaitUntilIdle(2000));
  CHECK_EQ(service.WrittenOk(), 2ULL);
  CHECK_EQ(CountJpg(dir), 2);
  service.Stop();
  aibox::media::AnomalySnapshotService::SetProcessInstance(nullptr);
  RemoveTree(dir);
  return true;
}

}  // namespace

int main() {
  gst_init(nullptr, nullptr);
  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"OneFileThreeReady", TestOneFileThreeReady},
      {"WriteFailurePublishesZero", TestWriteFailurePublishesZero},
      {"DropOldestBeforeStart", TestDropOldestBeforeStart},
      {"ThumbnailSuccess", TestThumbnailSuccess},
      {"ThumbnailFailureKeepsMessage", TestThumbnailFailureKeepsMessage},
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
