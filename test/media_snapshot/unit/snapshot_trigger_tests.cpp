#include <iostream>
#include <string>
#include <vector>

#include "aibox/aggregator/Result.hpp"
#include "aibox/media/SnapshotTriggerPolicy.hpp"

namespace {

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

#define CHECK_EQ(a, b) CHECK_TRUE((a) == (b))

aibox::aggregator::Detection Det(const std::string& id, const std::string& label,
                                 float conf) {
  aibox::aggregator::Detection d;
  d.event_id = id;
  d.label = label;
  d.confidence = conf;
  return d;
}

bool TestEmptyToNonEmptyTriggers() {
  aibox::media::SnapshotTriggerPolicy policy;
  const auto empty = policy.Evaluate("cam-001", {}, 0);
  CHECK_TRUE(!empty.should_snapshot);

  const auto first = policy.Evaluate(
      "cam-001", {Det("id-a", "person", 0.9f)}, 10);
  CHECK_TRUE(first.should_snapshot);
  CHECK_EQ(first.representative_event_id, std::string("id-a"));
  CHECK_EQ(first.event_ids.size(), static_cast<size_t>(1));
  return true;
}

bool TestSameSetDoesNotRetrigger() {
  aibox::media::SnapshotTriggerPolicy policy;
  CHECK_TRUE(policy.Evaluate("cam-001", {Det("1", "person", 0.8f)}, 0)
                 .should_snapshot);
  CHECK_TRUE(!policy.Evaluate("cam-001", {Det("2", "person", 0.7f)}, 2000)
                  .should_snapshot);
  return true;
}

bool TestSetChangeTriggersAfterHoldoff() {
  aibox::media::SnapshotTriggerPolicy policy;
  CHECK_TRUE(policy.Evaluate("cam-001", {Det("1", "person", 0.8f)}, 0)
                 .should_snapshot);
  const auto changed = policy.Evaluate(
      "cam-001", {Det("2", "helmet", 0.6f)}, 2000);
  CHECK_TRUE(changed.should_snapshot);
  CHECK_EQ(changed.representative_event_id, std::string("2"));
  return true;
}

bool TestNonEmptyToEmptyDoesNotWrite() {
  aibox::media::SnapshotTriggerPolicy policy;
  CHECK_TRUE(policy.Evaluate("cam-001", {Det("1", "person", 0.8f)}, 0)
                 .should_snapshot);
  CHECK_TRUE(!policy.Evaluate("cam-001", {}, 2000).should_snapshot);
  CHECK_TRUE(!policy.Evaluate("cam-001", {Det("2", "person", 0.5f)}, 3000)
                  .should_snapshot);
  return true;
}

bool TestTwoCamerasIndependent() {
  aibox::media::SnapshotTriggerPolicy policy;
  CHECK_TRUE(policy.Evaluate("cam-001", {Det("a", "person", 0.8f)}, 0)
                 .should_snapshot);
  CHECK_TRUE(policy.Evaluate("cam-002", {Det("b", "person", 0.8f)}, 10)
                 .should_snapshot);
  CHECK_TRUE(!policy.Evaluate("cam-001", {Det("c", "person", 0.7f)}, 20)
                  .should_snapshot);
  return true;
}

bool TestHoldoffKeepsLastWrittenAndCatchUp() {
  aibox::media::SnapshotTriggerPolicy policy(1000);
  CHECK_TRUE(policy.Evaluate("cam-001", {Det("1", "person", 0.8f)}, 0)
                 .should_snapshot);
  const auto during = policy.Evaluate(
      "cam-001",
      {Det("2", "person", 0.8f), Det("3", "helmet", 0.5f)}, 500);
  CHECK_TRUE(!during.should_snapshot);
  const auto catch_up = policy.Evaluate(
      "cam-001",
      {Det("4", "person", 0.8f), Det("5", "helmet", 0.5f)}, 1000);
  CHECK_TRUE(catch_up.should_snapshot);
  CHECK_EQ(catch_up.event_ids.size(), static_cast<size_t>(2));
  return true;
}

bool TestRepresentativeMaxConfidenceFirstOnTie() {
  aibox::media::SnapshotTriggerPolicy policy;
  const auto d = policy.Evaluate(
      "cam-001",
      {Det("first", "person", 0.7f), Det("best", "helmet", 0.9f),
       Det("tie", "smoke", 0.9f)},
      0);
  CHECK_TRUE(d.should_snapshot);
  CHECK_EQ(d.representative_event_id, std::string("best"));
  CHECK_EQ(d.event_ids.size(), static_cast<size_t>(3));
  return true;
}

}  // namespace

int main() {
  const struct {
    const char* name;
    bool (*fn)();
  } tests[] = {
      {"EmptyToNonEmptyTriggers", TestEmptyToNonEmptyTriggers},
      {"SameSetDoesNotRetrigger", TestSameSetDoesNotRetrigger},
      {"SetChangeTriggersAfterHoldoff", TestSetChangeTriggersAfterHoldoff},
      {"NonEmptyToEmptyDoesNotWrite", TestNonEmptyToEmptyDoesNotWrite},
      {"TwoCamerasIndependent", TestTwoCamerasIndependent},
      {"HoldoffKeepsLastWrittenAndCatchUp", TestHoldoffKeepsLastWrittenAndCatchUp},
      {"RepresentativeMaxConfidenceFirstOnTie",
       TestRepresentativeMaxConfidenceFirstOnTie},
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
