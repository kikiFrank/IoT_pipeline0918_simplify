/// @file result_store_tests.cpp
/// @brief Scenario 6+12: TTL/seq/epoch publish guards, sample-time-based age,
///        TOCTOU (FLUSH between precheck and publish), camera map hard cap.

#include <gtest/gtest.h>

#include <cstdint>

#include "async/result_store.hpp"
#include "test_helpers.hpp"

namespace fa = face_async;

TEST(ResultStoreTest, TtlBoundaryAtPublish) {
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  // fresh sample at t=0
  EXPECT_TRUE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 10, 1, 1, 1,
                           /*sample=*/0),
      1000));
  // age == ttl: still acceptable (boundary inclusive)
  clk.advance(1000);
  EXPECT_TRUE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_EMPTY, 11, 1, 1, 2,
                           /*sample=*/0),
      1000));
  // age == ttl+1: rejected (sample-time based, boundary exclusive)
  clk.advance(1);
  EXPECT_FALSE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 12, 1, 1, 3,
                           /*sample=*/0),
      1000));
}

TEST(ResultStoreTest, StaleAtPublishRejectedEvenIfJustCompleted) {
  // graphExecute stuck 10s: worker "completes" now but sample is 10s old.
  // The result must NOT be treated as fresh — completion time is never the basis.
  FakeClock clk;
  clk.us = 0;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  auto e = MakeEntry(fa::ResultStatus::VALID_FACES, 5, 1, 1, 1, /*sample_us=*/0);
  clk.us = 10'000'000;  // now, 10s later
  EXPECT_FALSE(store.publish_if_current("cam-001", e, 250'000));
}

TEST(ResultStoreTest, OldGenerationAndOldSeqNeverOverwriteNewer) {
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 5);
  EXPECT_TRUE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 1, 1, 5, 10, clk()),
      100000));
  // older seq: rejected, newer entry stays
  EXPECT_FALSE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_EMPTY, 0, 1, 5, 9, clk()),
      100000));
  auto cur = store.latest("cam-001", 100000);
  ASSERT_NE(cur, nullptr);
  EXPECT_EQ(cur->seq, 10u);
  // old generation (pre-FLUSH): rejected after reset_epoch
  store.reset_epoch(1, 6);
  EXPECT_FALSE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 2, 1, 5, 11, clk()),
      100000));
  // new generation with newer seq: accepted
  EXPECT_TRUE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 3, 1, 6, 12, clk()),
      100000));
}

TEST(ResultStoreTest, ToctouFlushBetweenPrecheckAndPublish) {
  // Worker prechecks outside the store lock; a FLUSH (reset_epoch + clear)
  // happens between precheck and publish. publish_if_current must atomically
  // reject — the old result must never reappear after the flush.
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  // step 1: worker "precheck" — reads epoch outside lock (simulated)
  const auto seen_caps = store.epoch_caps_gen();
  const auto seen_stream = store.epoch_stream_gen();
  EXPECT_EQ(seen_caps, 1u);
  EXPECT_EQ(seen_stream, 1u);
  // step 2: FLUSH arrives from another thread: clear + new epoch
  store.reset_epoch(1, 2);
  store.clear();
  // step 3: worker publishes a result captured under the OLD epoch
  auto old = MakeEntry(fa::ResultStatus::VALID_FACES, 7, seen_caps, seen_stream, 5,
                       clk());
  EXPECT_FALSE(store.publish_if_current("cam-001", old, 100000));
  EXPECT_EQ(store.latest("cam-001", 100000), nullptr);
}

TEST(ResultStoreTest, LatestChecksTtlAndAgeIsSampleBased) {
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  clk.us = 1000;
  EXPECT_TRUE(store.publish_if_current(
      "cam-001",
      MakeEntry(fa::ResultStatus::VALID_FACES, 1, 1, 1, 1, /*sample=*/1000),
      250'000));
  clk.us = 1000 + 250'000;  // age == ttl: valid
  std::int64_t age = -1;
  EXPECT_NE(store.latest("cam-001", 250'000, &age), nullptr);
  EXPECT_EQ(age, 250'000);
  clk.us = 1000 + 250'001;  // age > ttl: stale
  EXPECT_EQ(store.latest("cam-001", 250'000, &age), nullptr);
}

TEST(ResultStoreTest, CameraMapHardCap) {
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  for (int i = 0; i < static_cast<int>(fa::kMaxCameras); ++i) {
    EXPECT_TRUE(store.publish_if_current(
        "cam-" + std::to_string(i),
        MakeEntry(fa::ResultStatus::VALID_EMPTY, 1, 1, 1, i + 1, clk()), 100000));
  }
  EXPECT_FALSE(store.publish_if_current(
      "cam-overflow", MakeEntry(fa::ResultStatus::VALID_EMPTY, 1, 1, 1, 999, clk()),
      100000));
  EXPECT_EQ(store.latest("cam-overflow", 100000), nullptr);
}

TEST(ResultStoreTest, ClearRemovesEverything) {
  FakeClock clk;
  fa::ResultStore store([&clk] { return clk(); });
  store.reset_epoch(1, 1);
  EXPECT_TRUE(store.publish_if_current(
      "cam-001", MakeEntry(fa::ResultStatus::VALID_FACES, 1, 1, 1, 1, clk()),
      100000));
  store.clear();
  EXPECT_EQ(store.latest("cam-001", 100000), nullptr);
}
