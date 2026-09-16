#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "aibox/aggregator/Result.hpp"
#include "aibox/detection/FrameEventBuilder.hpp"
#include "aibox/detection/Uuid.hpp"
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

const std::regex kUuidV4(
    "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");

bool IsUuidV4(const std::string& value) {
  return std::regex_match(value, kUuidV4);
}

aibox::aggregator::AggregatedResult MakePresetResult() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-001";
  result.capture_timestamp_ms = 1750000000000ULL;
  result.frame_width = 1920;
  result.frame_height = 1080;

  aibox::aggregator::Detection person;
  person.event_id = "550e8400-e29b-41d4-a716-446655440000";
  person.label = "person";
  person.confidence = 0.95f;
  person.x = 120;
  person.y = 80;
  person.w = 160;
  person.h = 340;
  result.detections.push_back(person);

  aibox::aggregator::Detection helmet;
  helmet.event_id = "550e8400-e29b-41d4-a716-446655440001";
  helmet.label = "helmet";
  helmet.confidence = 0.72f;
  helmet.x = 150;
  helmet.y = 60;
  helmet.w = 70;
  helmet.h = 70;
  result.detections.push_back(helmet);
  return result;
}

bool TestPresetEventIdsAreReused() {
  const aibox::aggregator::AggregatedResult result = MakePresetResult();
  const aibox::detection::FrameEventBuilder builder;
  const aibox::FrameEvent frame = builder.BuildProto(result);

  CHECK_EQ(frame.detections_size(), 2);
  CHECK_EQ(frame.detections(0).event_id(), result.detections[0].event_id);
  CHECK_EQ(frame.detections(1).event_id(), result.detections[1].event_id);
  CHECK_EQ(frame.detections(0).event_id(),
           std::string("550e8400-e29b-41d4-a716-446655440000"));
  CHECK_EQ(frame.detections(1).event_id(),
           std::string("550e8400-e29b-41d4-a716-446655440001"));
  return true;
}

bool TestSameFrameIdsAreUniqueWhenGenerated() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-001";
  result.capture_timestamp_ms = 1750000001000ULL;
  result.detections.resize(3);
  result.detections[0].label = "Green";
  result.detections[1].label = "Human";
  result.detections[2].label = "helmet";

  const aibox::detection::FrameEventBuilder builder;
  const aibox::FrameEvent frame = builder.BuildProto(result);
  CHECK_EQ(frame.detections_size(), 3);

  std::set<std::string> ids;
  for (const auto& det : frame.detections()) {
    CHECK_TRUE(IsUuidV4(det.event_id()));
    CHECK_TRUE(ids.insert(det.event_id()).second);
  }
  CHECK_EQ(ids.size(), static_cast<size_t>(3));
  return true;
}

bool TestEmptyEventIdFallsBackToUuidV4() {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = "cam-002";
  result.capture_timestamp_ms = 1750000002000ULL;

  aibox::aggregator::Detection empty_id;
  empty_id.label = "person";
  empty_id.confidence = 0.5f;
  result.detections.push_back(empty_id);

  const aibox::detection::FrameEventBuilder builder;
  const aibox::FrameEvent frame = builder.BuildProto(result);
  CHECK_EQ(frame.detections_size(), 1);
  CHECK_TRUE(result.detections[0].event_id.empty());
  CHECK_TRUE(IsUuidV4(frame.detections(0).event_id()));
  return true;
}

bool TestUuidV4HelperFormat() {
  const std::string id = aibox::detection::UuidV4();
  CHECK_TRUE(IsUuidV4(id));
  return true;
}

}  // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"PresetEventIdsAreReused", TestPresetEventIdsAreReused},
      {"SameFrameIdsAreUniqueWhenGenerated", TestSameFrameIdsAreUniqueWhenGenerated},
      {"EmptyEventIdFallsBackToUuidV4", TestEmptyEventIdFallsBackToUuidV4},
      {"UuidV4HelperFormat", TestUuidV4HelperFormat},
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
