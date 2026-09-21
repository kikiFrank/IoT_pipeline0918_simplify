/// @file slot_pool_tests.cpp
/// @brief Scenario 1+2: SlotPool strict transitions, illegal ops keep state,
///        no forced INFERENCING->FREE, duplicate release rejected,
///        no-FREE-slot acquire returns immediately.

#include <gtest/gtest.h>

#include <cstdint>

#include "async/slot_pool.hpp"

namespace fa = face_async;

TEST(SlotPoolTest, FullLifecycleTransitions) {
  fa::SlotPool pool(4);
  std::uint32_t idx = 0;
  ASSERT_TRUE(pool.acquire(idx));
  EXPECT_EQ(pool.state(idx), fa::SlotState::COPYING);
  ASSERT_TRUE(pool.commit_copy(idx));
  EXPECT_EQ(pool.state(idx), fa::SlotState::QUEUED);
  ASSERT_TRUE(pool.begin_inference(idx));
  EXPECT_EQ(pool.state(idx), fa::SlotState::INFERENCING);
  ASSERT_TRUE(pool.complete_inference(idx));
  EXPECT_EQ(pool.state(idx), fa::SlotState::FREE);
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, IllegalTransitionsKeepStateAndAreRejected) {
  fa::SlotPool pool(2);
  std::uint32_t a = 0, b = 0;
  ASSERT_TRUE(pool.acquire(a));
  ASSERT_TRUE(pool.acquire(b));
  EXPECT_FALSE(pool.commit_copy(99));    // out of range
  EXPECT_TRUE(pool.commit_copy(a));      // COPYING->QUEUED ok
  EXPECT_FALSE(pool.commit_copy(a));     // double commit illegal
  EXPECT_EQ(pool.state(a), fa::SlotState::QUEUED);
  EXPECT_FALSE(pool.cancel_copy(a));     // cancel from QUEUED illegal
  EXPECT_TRUE(pool.evict(a));            // QUEUED->FREE via evict is the only legal path
  EXPECT_FALSE(pool.evict(a));           // FREE->FREE evict illegal
  EXPECT_FALSE(pool.begin_inference(b)); // COPYING->INFERENCING illegal
  EXPECT_EQ(pool.state(b), fa::SlotState::COPYING);
  EXPECT_FALSE(pool.complete_inference(b));  // COPYING->FREE via complete illegal
  EXPECT_TRUE(pool.cancel_copy(b));
  EXPECT_FALSE(pool.begin_inference(b));     // FREE->INFERENCING illegal
  EXPECT_GT(pool.counters().illegal, 0u);
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, ResetAllRefusesWhileInferencingAndNeverForcesState) {
  fa::SlotPool pool(3);
  std::uint32_t a = 0;
  ASSERT_TRUE(pool.acquire(a));
  ASSERT_TRUE(pool.commit_copy(a));
  ASSERT_TRUE(pool.begin_inference(a));
  // reset_all must refuse: an INFERENCING slot may still be touched by the worker
  EXPECT_FALSE(pool.reset_all());
  EXPECT_EQ(pool.state(a), fa::SlotState::INFERENCING);  // state kept, never forced
  ASSERT_TRUE(pool.complete_inference(a));
  EXPECT_TRUE(pool.reset_all());
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, ResetAllRefusesCopyingAndQueuedToo) {
  // reset_all is strict: it succeeds ONLY when every slot is already FREE.
  // COPYING and QUEUED are never forced back to FREE.
  fa::SlotPool pool(3);
  std::uint32_t a = 0, b = 0;
  ASSERT_TRUE(pool.acquire(a));
  ASSERT_TRUE(pool.acquire(b));
  ASSERT_TRUE(pool.commit_copy(b));
  EXPECT_FALSE(pool.reset_all());          // a is COPYING
  EXPECT_EQ(pool.state(a), fa::SlotState::COPYING);
  EXPECT_TRUE(pool.cancel_copy(a));
  EXPECT_FALSE(pool.reset_all());          // b is QUEUED
  EXPECT_EQ(pool.state(b), fa::SlotState::QUEUED);
  EXPECT_TRUE(pool.evict(b));
  EXPECT_TRUE(pool.reset_all());           // all FREE now
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, DuplicateReleaseRejected) {
  fa::SlotPool pool(2);
  std::uint32_t a = 0;
  ASSERT_TRUE(pool.acquire(a));
  ASSERT_TRUE(pool.commit_copy(a));
  ASSERT_TRUE(pool.begin_inference(a));
  EXPECT_TRUE(pool.complete_inference(a));
  EXPECT_FALSE(pool.complete_inference(a));  // duplicate release rejected
  EXPECT_EQ(pool.state(a), fa::SlotState::FREE);
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, AcquireWithNoFreeSlotReturnsImmediately) {
  fa::SlotPool pool(2);
  std::uint32_t a = 0, b = 0, c = 0;
  ASSERT_TRUE(pool.acquire(a));
  ASSERT_TRUE(pool.acquire(b));
  // no sleep, no wait: must return false at once (never blocks)
  EXPECT_FALSE(pool.acquire(c));
  EXPECT_GE(pool.counters().noslot, 1u);
  EXPECT_TRUE(pool.invariant_ok());
}

TEST(SlotPoolTest, CountersTrackEveryTransition) {
  fa::SlotPool pool(2);
  std::uint32_t a = 0;
  EXPECT_EQ(pool.counters().allocated, 2u);
  EXPECT_EQ(pool.counters().free_n, 2u);
  ASSERT_TRUE(pool.acquire(a));
  EXPECT_EQ(pool.counters().free_n, 1u);
  EXPECT_EQ(pool.counters().copying, 1u);
  ASSERT_TRUE(pool.commit_copy(a));
  EXPECT_EQ(pool.counters().copying, 0u);
  EXPECT_EQ(pool.counters().queued, 1u);
  ASSERT_TRUE(pool.begin_inference(a));
  EXPECT_EQ(pool.counters().queued, 0u);
  EXPECT_EQ(pool.counters().inferencing, 1u);
  ASSERT_TRUE(pool.complete_inference(a));
  EXPECT_EQ(pool.counters().inferencing, 0u);
  EXPECT_EQ(pool.counters().free_n, 2u);
  EXPECT_TRUE(pool.invariant_ok());
}
