#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "IsolatedNats.hpp"
#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/MediaReadyEvent.hpp"
#include "aibox/media/NatsMediaReadyTransport.hpp"
#include "aibox_contracts.pb.h"

namespace {

constexpr int kSkipNoNats = 77;
constexpr int kPort = 14225;

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

bool ExtractJsonString(const std::string& json, const std::string& key,
                       std::string& out) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  size_t i = pos + needle.size();
  out.clear();
  while (i < json.size()) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      out.push_back(json[i + 1]);
      i += 2;
      continue;
    }
    if (json[i] == '"') return true;
    out.push_back(json[i]);
    ++i;
  }
  return false;
}

bool ExtractJsonInt64(const std::string& json, const std::string& key,
                      int64_t& out) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  size_t i = pos + needle.size();
  char* end = nullptr;
  const long long value = std::strtoll(json.c_str() + i, &end, 10);
  if (end == json.c_str() + i) return false;
  out = static_cast<int64_t>(value);
  return true;
}

bool WaitForConnect(aibox::adapter::NatsAdapter& nats,
                    const aibox::adapter::NatsAdapter::Config& cfg,
                    int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (nats.Connect(cfg)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WriteThenPublish(aibox::media::MediaFileWriter& writer,
                      aibox::media::IMediaReadyTransport& transport,
                      aibox::media::MediaReadyEvent ev,
                      const std::vector<uint8_t>& jpeg) {
  const auto wr = writer.WriteJpeg(ev.event_id, jpeg);
  if (!wr.ok) return false;
  ev.file_path = wr.absolute_path;
  ev.size_bytes = wr.size_bytes;
  return transport.PublishReady(ev.ToJson());
}

bool TestSuccessPublishesFiveFields() {
  const std::string dir = MakeTempDir("pubok");
  CHECK_TRUE(!dir.empty());

  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  cfg.client_name = "aibox-media-pub-ok";
  cfg.max_pending = 0;

  aibox::adapter::NatsAdapter subscriber;
  CHECK_TRUE(WaitForConnect(subscriber, cfg, 30));

  std::mutex mu;
  std::condition_variable cv;
  int received = 0;
  std::string last_subject;
  std::string last_payload;
  CHECK_TRUE(subscriber.Subscribe(
      aibox::media::NatsMediaReadyTransport::kSubject,
      [&](const std::string& subject, const std::string&,
          const std::string& payload) {
        std::lock_guard<std::mutex> lock(mu);
        last_subject = subject;
        last_payload = payload;
        ++received;
        cv.notify_all();
      }));

  aibox::adapter::NatsAdapter publisher_nats;
  cfg.client_name = "aibox-media-pub-ok-pub";
  CHECK_TRUE(WaitForConnect(publisher_nats, cfg, 10));
  aibox::media::NatsMediaReadyTransport transport(publisher_nats);
  aibox::media::MediaFileWriter writer(dir);

  const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0x11, 0x22, 0xFF, 0xD9};
  aibox::media::MediaReadyEvent ev;
  ev.event_id = "550e8400-e29b-41d4-a716-4466554400aa";
  ev.camera_id = "cam-001";
  ev.timestamp_ms = 1750000000123LL;
  CHECK_TRUE(WriteThenPublish(writer, transport, ev, jpeg));

  {
    std::unique_lock<std::mutex> lock(mu);
    CHECK_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                           [&] { return received >= 1; }));
  }

  CHECK_EQ(last_subject,
           std::string(aibox::media::NatsMediaReadyTransport::kSubject));
  CHECK_TRUE(!last_payload.empty());
  CHECK_EQ(last_payload.front(), '{');

  aibox::FrameEvent proto;
  CHECK_TRUE(!proto.ParseFromArray(last_payload.data(),
                                   static_cast<int>(last_payload.size())) ||
             proto.camera_id().empty());

  std::string event_id;
  std::string file_path;
  std::string camera_id;
  int64_t size_bytes = 0;
  int64_t timestamp_ms = 0;
  CHECK_TRUE(ExtractJsonString(last_payload, "event_id", event_id));
  CHECK_TRUE(ExtractJsonString(last_payload, "file_path", file_path));
  CHECK_TRUE(ExtractJsonString(last_payload, "camera_id", camera_id));
  CHECK_TRUE(ExtractJsonInt64(last_payload, "size_bytes", size_bytes));
  CHECK_TRUE(ExtractJsonInt64(last_payload, "timestamp_ms", timestamp_ms));
  CHECK_EQ(event_id, ev.event_id);
  CHECK_EQ(camera_id, ev.camera_id);
  CHECK_EQ(timestamp_ms, ev.timestamp_ms);
  CHECK_TRUE(!file_path.empty() && file_path[0] == '/');
  CHECK_TRUE(FileExists(file_path));
  CHECK_EQ(size_bytes, static_cast<int64_t>(jpeg.size()));

  std::ifstream in(file_path, std::ios::binary);
  std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK_TRUE(got == jpeg);

  subscriber.Disconnect();
  publisher_nats.Disconnect();
  RemoveTree(dir);
  return true;
}

bool TestWriteFailurePublishesNothing() {
  const std::string dir = MakeTempDir("pubfail");
  CHECK_TRUE(!dir.empty());
  CHECK_EQ(chmod(dir.c_str(), 0555), 0);

  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  cfg.client_name = "aibox-media-pub-fail";
  cfg.max_pending = 0;

  aibox::adapter::NatsAdapter subscriber;
  CHECK_TRUE(WaitForConnect(subscriber, cfg, 30));

  std::mutex mu;
  int received = 0;
  CHECK_TRUE(subscriber.Subscribe(
      aibox::media::NatsMediaReadyTransport::kSubject,
      [&](const std::string&, const std::string&, const std::string&) {
        std::lock_guard<std::mutex> lock(mu);
        ++received;
      }));

  aibox::adapter::NatsAdapter publisher_nats;
  cfg.client_name = "aibox-media-pub-fail-pub";
  CHECK_TRUE(WaitForConnect(publisher_nats, cfg, 10));
  aibox::media::NatsMediaReadyTransport transport(publisher_nats);
  aibox::media::MediaFileWriter writer(dir);

  const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
  aibox::media::MediaReadyEvent ev;
  ev.event_id = "550e8400-e29b-41d4-a716-4466554400bb";
  ev.camera_id = "cam-001";
  ev.timestamp_ms = 1750000000456LL;
  CHECK_TRUE(!WriteThenPublish(writer, transport, ev, jpeg));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  {
    std::lock_guard<std::mutex> lock(mu);
    CHECK_EQ(received, 0);
  }
  CHECK_TRUE(!FileExists(dir + "/" + ev.event_id + ".jpg"));
  CHECK_TRUE(!FileExists(dir + "/" + ev.event_id + ".jpg.tmp"));

  subscriber.Disconnect();
  publisher_nats.Disconnect();
  (void)chmod(dir.c_str(), 0755);
  RemoveTree(dir);
  return true;
}

}  // namespace

int main() {
  const std::string bin = aibox::test::FindNatsServer();
  if (bin.empty()) {
    std::cerr << "SKIP nats-server not found\n";
    return kSkipNoNats;
  }
  if (aibox::test::PortOpen(kPort)) {
    std::cerr << "SKIP port " << kPort << " already in use\n";
    return kSkipNoNats;
  }

  const pid_t pid = aibox::test::StartIsolatedNats(bin, kPort, "aibox-media-itest");
  if (pid <= 0 || !aibox::test::WaitForPort(kPort, 40)) {
    aibox::test::StopIsolatedNats(pid);
    std::cerr << "SKIP isolated nats on " << kPort << " failed to start\n";
    return kSkipNoNats;
  }

  int rc = 0;
  if (!TestSuccessPublishesFiveFields()) {
    std::cerr << "FAILED TestSuccessPublishesFiveFields\n";
    rc = 1;
  } else {
    std::cout << "PASSED TestSuccessPublishesFiveFields\n";
  }
  if (rc == 0 && !TestWriteFailurePublishesNothing()) {
    std::cerr << "FAILED TestWriteFailurePublishesNothing\n";
    rc = 1;
  } else if (rc == 0) {
    std::cout << "PASSED TestWriteFailurePublishesNothing\n";
  }

  aibox::test::StopIsolatedNats(pid);
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();
  return rc;
}
