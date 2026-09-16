#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <vector>

#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/detection/CaptureTimestampTracker.hpp"
#include "aibox/detection/FrameEventBuilder.hpp"
#include "aibox/detection/FrameEventPublisher.hpp"
#include "aibox/detection/IFrameTransport.hpp"
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

bool Near(float a, float b) {
  return std::fabs(a - b) < 0.0001f;
}

aibox::aggregator::AggregatedResult MakeResultWithDetections() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-001";
  result.frame_id = 42;
  result.capture_timestamp_ms = 1750000000000ULL;
  result.frame_width = 1920;
  result.frame_height = 1080;

  aibox::aggregator::Detection person;
  person.label = "person";
  person.confidence = 95.0f;
  person.x = 120;
  person.y = 80;
  person.w = 160;
  person.h = 340;
  result.detections.push_back(person);

  aibox::aggregator::Detection helmet;
  helmet.label = "helmet";
  helmet.confidence = 72.0f;
  helmet.x = 1900;
  helmet.y = 1070;
  helmet.w = 100;
  helmet.h = 100;
  result.detections.push_back(helmet);
  return result;
}

bool TestFrameEventContractMapping() {
  const aibox::aggregator::AggregatedResult result = MakeResultWithDetections();
  const aibox::detection::FrameEventBuilder builder;
  const std::vector<uint8_t> bytes = builder.Serialize(result);
  CHECK_TRUE(!bytes.empty());

  aibox::FrameEvent frame;
  CHECK_TRUE(frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  CHECK_EQ(frame.camera_id(), "cam-001");
  CHECK_EQ(frame.timestamp_ms(), 1750000000000LL);
  CHECK_EQ(frame.detections_size(), 2);

  const auto& person = frame.detections(0);
  CHECK_EQ(person.class_id(), "person");
  CHECK_TRUE(Near(person.confidence(), 0.95f));
  CHECK_TRUE(Near(person.x_min(), 120.0f));
  CHECK_TRUE(Near(person.y_min(), 80.0f));
  CHECK_TRUE(Near(person.x_max(), 280.0f));
  CHECK_TRUE(Near(person.y_max(), 420.0f));

  const auto& helmet = frame.detections(1);
  CHECK_EQ(helmet.class_id(), "helmet");
  CHECK_TRUE(Near(helmet.confidence(), 0.72f));
  CHECK_TRUE(Near(helmet.x_max(), 1920.0f));
  CHECK_TRUE(Near(helmet.y_max(), 1080.0f));
  return true;
}

bool TestEmptyFrameIsPublished() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-empty";
  result.capture_timestamp_ms = 1750000001000ULL;

  const aibox::detection::FrameEventBuilder builder;
  const std::vector<uint8_t> bytes = builder.Serialize(result);
  CHECK_TRUE(!bytes.empty());

  aibox::FrameEvent frame;
  CHECK_TRUE(frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  CHECK_EQ(frame.camera_id(), "cam-empty");
  CHECK_EQ(frame.timestamp_ms(), 1750000001000LL);
  CHECK_EQ(frame.detections_size(), 0);
  return true;
}

bool TestUuidV4Uniqueness() {
  const aibox::aggregator::AggregatedResult result = MakeResultWithDetections();
  const aibox::detection::FrameEventBuilder builder;
  const std::regex uuid_v4(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  std::set<std::string> ids;

  for (int i = 0; i < 64; ++i) {
    const aibox::FrameEvent frame = builder.BuildProto(result);
    for (const auto& detection : frame.detections()) {
      CHECK_TRUE(std::regex_match(detection.event_id(), uuid_v4));
      CHECK_TRUE(ids.insert(detection.event_id()).second);
    }
  }
  return true;
}

bool TestAggregatorWaitPopNext() {
  aibox::aggregator::ResultAggregator aggregator;
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-002";
  result.frame_id = 7;
  CHECK_TRUE(aggregator.Submit(std::move(result)));

  aibox::aggregator::AggregatedResult out;
  CHECK_TRUE(aggregator.WaitPopNext(out, std::chrono::milliseconds(10)));
  CHECK_EQ(out.camera_id, "cam-002");
  CHECK_EQ(out.frame_id, 7ULL);
  CHECK_TRUE(!aggregator.WaitPopNext(out, std::chrono::milliseconds(1)));
  return true;
}

class FakeTransport final : public aibox::detection::IFrameTransport {
 public:
  bool PublishFrame(const std::vector<uint8_t>& payload) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      payloads_.push_back(payload);
    }
    cv_.notify_all();
    return true;
  }

  bool WaitForPayloadCount(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count] {
      return payloads_.size() >= count;
    });
  }

  std::vector<uint8_t> PayloadAt(size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return payloads_.at(index);
  }

  size_t PayloadCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return payloads_.size();
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::vector<uint8_t>> payloads_;
};

bool TestFrameEventPublisherUsesTransport() {
  aibox::aggregator::ResultAggregator aggregator;
  FakeTransport transport;
  aibox::detection::FrameEventPublisher publisher(aggregator, transport);
  publisher.Start();

  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-003";
  result.capture_timestamp_ms = 1750000002000ULL;
  CHECK_TRUE(aggregator.Submit(std::move(result)));

  CHECK_TRUE(transport.WaitForPayloadCount(1, std::chrono::milliseconds(500)));
  publisher.Stop();

  CHECK_EQ(transport.PayloadCount(), static_cast<size_t>(1));
  const std::vector<uint8_t> payload = transport.PayloadAt(0);
  aibox::FrameEvent frame;
  CHECK_TRUE(frame.ParseFromArray(payload.data(), static_cast<int>(payload.size())));
  CHECK_EQ(frame.camera_id(), "cam-003");
  CHECK_EQ(frame.detections_size(), 0);
  CHECK_EQ(publisher.Published(), 1ULL);
  return true;
}

bool TestCaptureTimestampTracker() {
  aibox::detection::CaptureTimestampTracker tracker;
  tracker.Record("cam-001", 1, 1000);
  tracker.Record("cam-002", 1, 2000);
  CHECK_EQ(tracker.Take("cam-001", 1), 1000ULL);
  CHECK_EQ(tracker.Take("cam-002", 1), 2000ULL);
  CHECK_EQ(tracker.Misses(), 0ULL);

  const uint64_t miss_ts = tracker.Take("cam-001", 99);
  CHECK_TRUE(miss_ts >= 1000);
  CHECK_EQ(tracker.Misses(), 1ULL);

  tracker.Reset();
  tracker.Record("cam-001", 5, 5000);
  tracker.Record("cam-001", 6, 4000);
  CHECK_EQ(tracker.Take("cam-001", 5), 5000ULL);
  CHECK_EQ(tracker.Take("cam-001", 6), 5001ULL);
  CHECK_EQ(tracker.RegressFixes(), 1ULL);

  tracker.Reset();
  for (uint64_t i = 1; i <= 300; ++i) {
    tracker.Record("cam-001", i, 10000 + i);
  }
  CHECK_EQ(tracker.Take("cam-001", 300), 10300ULL);
  const uint64_t evicted = tracker.Take("cam-001", 1);
  CHECK_TRUE(evicted >= 10300ULL);
  CHECK_EQ(tracker.Misses(), 1ULL);

  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-001";
  result.capture_timestamp_ms = 1750000000123ULL;
  const aibox::detection::FrameEventBuilder builder;
  const aibox::FrameEvent frame = builder.BuildProto(result);
  CHECK_EQ(frame.timestamp_ms(), 1750000000123LL);
  return true;
}

bool TestUuidGenerationThroughput() {
  aibox::aggregator::AggregatedResult result = MakeResultWithDetections();
  const aibox::detection::FrameEventBuilder builder;
  const auto start = std::chrono::steady_clock::now();
  std::set<std::string> ids;
  constexpr int kFrames = 5000;
  for (int i = 0; i < kFrames; ++i) {
    const aibox::FrameEvent frame = builder.BuildProto(result);
    CHECK_EQ(frame.detections_size(), 2);
    CHECK_TRUE(ids.insert(frame.detections(0).event_id()).second);
    CHECK_TRUE(ids.insert(frame.detections(1).event_id()).second);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  std::cout << "[NFR-4] 10000 UUIDs in " << ms << " ms" << std::endl;
  CHECK_TRUE(ids.size() == 10000);
  CHECK_TRUE(ms < 1000);
  return true;
}

}  // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"FrameEventContractMapping", TestFrameEventContractMapping},
      {"EmptyFrameIsPublished", TestEmptyFrameIsPublished},
      {"UuidV4Uniqueness", TestUuidV4Uniqueness},
      {"AggregatorWaitPopNext", TestAggregatorWaitPopNext},
      {"FrameEventPublisherUsesTransport", TestFrameEventPublisherUsesTransport},
      {"CaptureTimestampTracker", TestCaptureTimestampTracker},
      {"UuidGenerationThroughput", TestUuidGenerationThroughput},
  };

  for (const auto& test : tests) {
    if (!test.fn()) {
      std::cerr << "FAILED " << test.name << '\n';
      return 1;
    }
    std::cout << "PASSED " << test.name << '\n';
  }

  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
