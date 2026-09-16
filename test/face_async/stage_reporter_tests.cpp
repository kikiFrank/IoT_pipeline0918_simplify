/// @file stage_reporter_tests.cpp
/// @brief StageReporter enter/leave counters, current stage, injected clock.

#include <gtest/gtest.h>

#include <cstdint>

#include "async/stage_reporter.hpp"
#include "test_helpers.hpp"

namespace fa = face_async;

TEST(StageReporterTest, EnterLeaveCountersAndCurrent) {
  FakeClock clk;
  fa::StageReporter sr([&clk] { return clk(); });
  clk.us = 1000;
  sr.enter(fa::FaceStage::GRAPH_EXECUTE);
  EXPECT_EQ(sr.current(), fa::FaceStage::GRAPH_EXECUTE);
  EXPECT_EQ(sr.current_enter_us(), 1000);
  clk.us = 1500;
  sr.leave(fa::FaceStage::GRAPH_EXECUTE);
  EXPECT_EQ(sr.current(), fa::FaceStage::IDLE);
  auto st = sr.stats(fa::FaceStage::GRAPH_EXECUTE);
  EXPECT_EQ(st.enter_count, 1u);
  EXPECT_EQ(st.exit_count, 1u);
  EXPECT_EQ(st.total_us, 500u);
}

TEST(StageReporterTest, LeaveWithoutEnterIsHarmless) {
  FakeClock clk;
  fa::StageReporter sr([&clk] { return clk(); });
  sr.leave(fa::FaceStage::CL_FINISH);  // must not crash / not corrupt
  auto st = sr.stats(fa::FaceStage::CL_FINISH);
  EXPECT_EQ(st.enter_count, 0u);
  EXPECT_EQ(st.exit_count, 0u);
  EXPECT_EQ(sr.current(), fa::FaceStage::IDLE);
}

TEST(StageReporterTest, NestedEnterKeepsCurrentButCounts) {
  FakeClock clk;
  fa::StageReporter sr([&clk] { return clk(); });
  clk.us = 100;
  sr.enter(fa::FaceStage::COPY_INPUT);
  clk.us = 200;
  sr.enter(fa::FaceStage::DMA_SYNC);  // nested: current stays DMA_SYNC
  EXPECT_EQ(sr.current(), fa::FaceStage::DMA_SYNC);
  clk.us = 300;
  sr.leave(fa::FaceStage::DMA_SYNC);
  EXPECT_EQ(sr.current(), fa::FaceStage::IDLE);
  sr.leave(fa::FaceStage::COPY_INPUT);
  EXPECT_EQ(sr.stats(fa::FaceStage::COPY_INPUT).enter_count, 1u);
  EXPECT_EQ(sr.stats(fa::FaceStage::DMA_SYNC).enter_count, 1u);
}
