#include "aibox/media/AnomalySnapshotService.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace aibox::media;
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << '\n'; ++failures; } } while (0)

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

class Encoder final : public ISnapshotEncoder {
 public:
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& jpeg) override {
    ++calls;
    jpeg = {0xff, 0xd8, 0xff, 0xd9};
    return true;
  }
  std::atomic<int> calls{0};
};
class Thumbnail final : public IThumbnailEncoder {
 public:
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& jpeg) override {
    jpeg = {0xff, 0xd8, 0xff, 0xd9};
    return true;
  }
};
class Transport final : public IMediaReadyTransport {
 public:
  bool PublishReady(const std::string& json) override {
    std::lock_guard<std::mutex> lock(mu);
    messages.push_back(json);
    return true;
  }
  std::mutex mu;
  std::vector<std::string> messages;
};
bool Has(const std::string& json, const std::string& key, const std::string& value) {
  return json.find("\"" + key + "\":\"" + value + "\"") != std::string::npos;
}

void ReuseReportsActualFrame(const std::string& dir) {
  Encoder encoder;
  Thumbnail thumbnail;
  Transport transport;
  FrameSnapshotCache cache;
  const int64_t actual = NowMs() - 100;
  CachedFrame frame;
  frame.camera_id = "cam";
  frame.frame_id = "actual";
  frame.timestamp_ms = actual;
  frame.width = 2;
  frame.height = 2;
  frame.packed_nv12.assign(6, 1);
  CHECK(cache.Put(std::move(frame)));
  AnomalySnapshotService service(encoder, thumbnail, MediaFileWriter(dir), transport);
  service.SetFrameCache(&cache);
  service.Start();
  CHECK(service.EnqueueAlarmSnapshotRequest({"alarm-a", "cam", "missing", actual + 50, ""}));
  CHECK(service.WaitUntilIdle(3000));
  CHECK(service.EnqueueAlarmSnapshotRequest({"alarm-b", "cam", "missing", actual + 50, ""}));
  CHECK(service.WaitUntilIdle(3000));
  service.Stop();
  CHECK(encoder.calls.load() == 1);
  CHECK(transport.messages.size() == 2);
  if (transport.messages.size() == 2) {
    const std::string id = "cam-t" + std::to_string(actual);
    CHECK(Has(transport.messages[0], "alarm_id", "alarm-a"));
    CHECK(Has(transport.messages[1], "alarm_id", "alarm-b"));
    for (const auto& msg : transport.messages) {
      CHECK(Has(msg, "event_id", id));
      CHECK(Has(msg, "frame_id", "actual"));
      CHECK(msg.find("\"timestamp_ms\":" + std::to_string(actual)) != std::string::npos);
      CHECK(Has(msg, "file_path", dir + "/" + id + ".jpg"));
    }
  }
}

void StopRejectsAndReportsRequests(const std::string& dir) {
  Encoder encoder;
  Thumbnail thumbnail;
  Transport transport;
  AnomalySnapshotService service(encoder, thumbnail, MediaFileWriter(dir), transport);
  std::mutex mu;
  std::map<std::string, int> failures_by_id;
  service.SetFailureSink([&](const std::string& id, const std::string&) {
    std::lock_guard<std::mutex> lock(mu);
    ++failures_by_id[id];
  });
  service.Start();
  std::atomic<bool> go{false};
  std::thread producer([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
      service.EnqueueAlarmSnapshotRequest({"race-" + std::to_string(i), "cam", "", NowMs(), ""});
    }
  });
  go.store(true);
  service.Stop();
  producer.join();
  CHECK(!service.EnqueueAlarmSnapshotRequest({"after-stop", "cam", "", NowMs(), ""}));
  CHECK(service.WaitUntilIdle(100));
  std::lock_guard<std::mutex> lock(mu);
  CHECK(failures_by_id["after-stop"] == 1);
  for (int i = 0; i < 100; ++i) CHECK(failures_by_id["race-" + std::to_string(i)] == 1);
}
}  // namespace

int main() {
  char dir[] = "/tmp/aibox-snapshot-test-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  ReuseReportsActualFrame(dir);
  StopRejectsAndReportsRequests(dir);
  std::filesystem::remove_all(dir);
  if (failures) return 1;
  std::cout << "SNAPSHOT_SERVICE_TEST=PASS\n";
  return 0;
}
