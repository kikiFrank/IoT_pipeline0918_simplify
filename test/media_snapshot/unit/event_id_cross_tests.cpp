#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "aibox/aggregator/Result.hpp"
#include "aibox/detection/FrameEventBuilder.hpp"
#include "aibox/media/AnomalySnapshotService.hpp"
#include "aibox/media/IMediaReadyTransport.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/SnapshotEncoder.hpp"
#include "aibox/media/ThumbnailEncoder.hpp"
#include "aibox_contracts.pb.h"

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
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& jpeg_out) override {
    jpeg_out = {0xFF, 0xD8, 0xAA, 0xFF, 0xD9};
    return true;
  }
};

class FakeThumbnailEncoder final : public aibox::media::IThumbnailEncoder {
 public:
  bool EncodeNv12(const uint8_t*, uint32_t, uint32_t,
                  std::vector<uint8_t>& thumb_out) override {
    thumb_out = {0xFF, 0xD8, 0xBB, 0xFF, 0xD9};
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

std::string MakeTempDir() {
  char tmpl[] = "/tmp/aibox_media_cross_XXXXXX";
  char* created = mkdtemp(tmpl);
  return created ? std::string(created) : std::string();
}

void RemoveTree(const std::string& dir) {
  if (dir.empty() || dir.find("/tmp/aibox_media_") != 0) return;
  const int rc = std::system(("rm -rf \"" + dir + "\"").c_str());
  (void)rc;
}

bool ExtractJsonString(const std::string& json, const std::string& key,
                       std::string& out) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  size_t i = pos + needle.size();
  out.clear();
  while (i < json.size() && json[i] != '"') {
    out.push_back(json[i]);
    ++i;
  }
  return i < json.size();
}

bool TestPublishedIdsMatchFrameEvent() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-001";
  result.capture_timestamp_ms = 1750000000777ULL;
  result.frame_width = 64;
  result.frame_height = 48;

  aibox::aggregator::Detection a;
  a.event_id = "11111111-1111-4111-8111-111111111111";
  a.label = "person";
  a.confidence = 0.9f;
  result.detections.push_back(a);
  aibox::aggregator::Detection b;
  b.event_id = "22222222-2222-4222-8222-222222222222";
  b.label = "helmet";
  b.confidence = 0.4f;
  result.detections.push_back(b);

  const aibox::detection::FrameEventBuilder builder;
  const aibox::FrameEvent frame = builder.BuildProto(result);
  CHECK_EQ(frame.detections_size(), 2);
  CHECK_EQ(frame.detections(0).event_id(), result.detections[0].event_id);
  CHECK_EQ(frame.detections(1).event_id(), result.detections[1].event_id);

  const std::string dir = MakeTempDir();
  CHECK_TRUE(!dir.empty());
  FakeEncoder encoder;
  FakeThumbnailEncoder thumbnail_encoder;
  FakeTransport transport;
  aibox::media::MediaFileWriter writer(dir);
  aibox::media::AnomalySnapshotService service(
      encoder, thumbnail_encoder, writer, transport, 2);
  aibox::media::AnomalySnapshotService::SetProcessInstance(&service);
  service.Start();

  aibox::media::SnapshotTask task;
  task.camera_id = result.camera_id;
  task.timestamp_ms = static_cast<int64_t>(result.capture_timestamp_ms);
  task.representative_event_id = result.detections[0].event_id;
  for (const auto& det : result.detections) task.event_ids.push_back(det.event_id);
  task.packed_nv12.assign(64 * 48 * 3 / 2, 70);
  task.width = 64;
  task.height = 48;
  CHECK_TRUE(service.Enqueue(std::move(task)));
  CHECK_TRUE(service.WaitUntilIdle(2000));

  std::set<std::string> proto_ids;
  for (const auto& det : frame.detections()) proto_ids.insert(det.event_id());
  std::set<std::string> ready_ids;
  CHECK_EQ(transport.payloads.size(), static_cast<size_t>(2));
  for (const auto& payload : transport.payloads) {
    std::string event_id;
    CHECK_TRUE(ExtractJsonString(payload, "event_id", event_id));
    ready_ids.insert(event_id);
  }
  CHECK_TRUE(ready_ids == proto_ids);

  service.Stop();
  aibox::media::AnomalySnapshotService::SetProcessInstance(nullptr);
  RemoveTree(dir);
  return true;
}

}  // namespace

int main() {
  gst_init(nullptr, nullptr);
  if (!TestPublishedIdsMatchFrameEvent()) {
    std::cerr << "FAILED PublishedIdsMatchFrameEvent\n";
    return 1;
  }
  std::cout << "PASSED PublishedIdsMatchFrameEvent\n";
  return 0;
}
