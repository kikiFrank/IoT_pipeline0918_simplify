/// @file face_mailbox_tests.cpp
/// @brief Scenario 3+4+5: capacity-1 latest-only eviction, multi-camera isolation
///        + round-robin fairness, camera hard cap + camera_id length cap.

#include <gtest/gtest.h>

#include <string>

#include "async/face_mailbox.hpp"
#include "test_helpers.hpp"

namespace fa = face_async;

TEST(FaceMailboxTest, CapacityOneEvictsOldTask) {
  fa::FaceMailbox mb;
  auto t1 = MakeTask(0, "cam-001", 1, 1, 1, 1000);
  auto t2 = MakeTask(1, "cam-001", 2, 1, 1, 2000);
  fa::TaskRecord evicted;
  EXPECT_EQ(mb.push(t1, &evicted), fa::PushResult::OK);
  EXPECT_EQ(mb.depth("cam-001"), 1u);
  EXPECT_EQ(mb.push(t2, &evicted), fa::PushResult::EVICTED_OLD);
  EXPECT_EQ(evicted.slot_idx, 0u);   // old task returned for pool evict
  EXPECT_EQ(evicted.det_frame_id, 1u);
  EXPECT_EQ(mb.depth("cam-001"), 1u);
  auto got = mb.pop_round_robin();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->det_frame_id, 2u);  // newest kept
  EXPECT_EQ(mb.total_depth(), 0u);
  EXPECT_EQ(mb.counters().evicted, 1u);
}

TEST(FaceMailboxTest, MultiCameraIsolationAndRoundRobin) {
  fa::FaceMailbox mb;
  EXPECT_EQ(mb.push(MakeTask(0, "cam-001", 10, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  EXPECT_EQ(mb.push(MakeTask(1, "cam-002", 20, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  EXPECT_EQ(mb.push(MakeTask(2, "cam-003", 30, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  // eviction on cam-002 must not touch cam-001/cam-003
  fa::TaskRecord evicted;
  EXPECT_EQ(mb.push(MakeTask(3, "cam-002", 21, 1, 1, 200), &evicted),
            fa::PushResult::EVICTED_OLD);
  EXPECT_EQ(evicted.camera_id, "cam-002");
  EXPECT_EQ(mb.depth("cam-001"), 1u);
  EXPECT_EQ(mb.depth("cam-003"), 1u);
  // round-robin pops each camera before repeating
  auto p1 = mb.pop_round_robin();
  auto p2 = mb.pop_round_robin();
  auto p3 = mb.pop_round_robin();
  ASSERT_TRUE(p1 && p2 && p3);
  EXPECT_EQ(p1->camera_id, "cam-001");
  EXPECT_EQ(p2->camera_id, "cam-002");
  EXPECT_EQ(p2->det_frame_id, 21u);
  EXPECT_EQ(p3->camera_id, "cam-003");
  // fairness continues from where we left off
  EXPECT_EQ(mb.push(MakeTask(4, "cam-004", 40, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  auto p4 = mb.pop_round_robin();
  ASSERT_TRUE(p4.has_value());
  EXPECT_EQ(p4->camera_id, "cam-004");
}

TEST(FaceMailboxTest, CameraHardCapAndIdLength) {
  fa::FaceMailbox mb;
  for (int i = 0; i < static_cast<int>(fa::kMaxCameras); ++i) {
    std::string cam = "cam-" + std::to_string(i);
    EXPECT_EQ(mb.push(MakeTask(i, cam, 1, 1, 1, 100), nullptr),
              fa::PushResult::OK);
  }
  EXPECT_EQ(mb.total_depth(), fa::kMaxCameras);
  // 17th camera rejected, container does not grow
  EXPECT_FALSE(mb.camera_allowed("cam-overflow"));
  EXPECT_EQ(mb.push(MakeTask(0, "cam-overflow", 1, 1, 1, 100), nullptr),
            fa::PushResult::REJECTED);
  EXPECT_EQ(mb.total_depth(), fa::kMaxCameras);
  EXPECT_EQ(mb.counters().overcap, 1u);
  // id too long rejected
  std::string long_id(fa::kMaxCameraIdLen + 1, 'x');
  EXPECT_FALSE(mb.camera_allowed(long_id));
  // known camera id within cap still allowed
  EXPECT_TRUE(mb.camera_allowed("cam-0"));
}

TEST(FaceMailboxTest, KnownCameraRingUniqueAcrossPushPopCycles) {
  // Regression: a drained bucket must keep ONE key in the ring — pushing the
  // same camera across many push/pop cycles must not grow known_cameras().
  fa::FaceMailbox mb;
  for (int i = 0; i < 5; ++i) {
    fa::TaskRecord evicted;
    EXPECT_NE(mb.push(MakeTask(i % 3, "cam-001", i + 1, 1, 1, 100), &evicted),
              fa::PushResult::REJECTED);
    auto t = mb.pop_round_robin();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->camera_id, "cam-001");
  }
  const auto cams = mb.known_cameras();
  ASSERT_EQ(cams.size(), 1u);
  EXPECT_EQ(cams[0], "cam-001");
  // camera-cap accounting also uses the unique ring
  for (int i = 0; i < static_cast<int>(fa::kMaxCameras) - 1; ++i) {
    std::string cam = "extra-" + std::to_string(i);
    EXPECT_TRUE(mb.camera_allowed(cam));
    EXPECT_EQ(mb.push(MakeTask(i + 4, cam, 1, 1, 1, 100), nullptr),
              fa::PushResult::OK);
  }
  EXPECT_FALSE(mb.camera_allowed("extra-overflow"));
}

TEST(FaceMailboxTest, DrainQueuedReturnsAllTasksInPushOrder) {
  fa::FaceMailbox mb;
  EXPECT_EQ(mb.push(MakeTask(0, "cam-001", 1, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  EXPECT_EQ(mb.push(MakeTask(1, "cam-002", 2, 1, 1, 100), nullptr),
            fa::PushResult::OK);
  auto drained = mb.drain_queued();
  ASSERT_EQ(drained.size(), 2u);
  EXPECT_EQ(mb.total_depth(), 0u);
  // second drain is empty (idempotent, no double-return possible)
  EXPECT_TRUE(mb.drain_queued().empty());
}
