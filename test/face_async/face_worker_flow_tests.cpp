/// @file face_worker_flow_tests.cpp
/// @brief Scenario 7+8+11+13 + expert round-2 regressions:
///        FLUSH reclaims only QUEUED, copy-during-FLUSH cancelled with the
///        EXACT in-copy-window race (epoch captured at acquire), five-flag
///        accepting matrix, FLUSH_STOP/STREAM_START authority rules,
///        EOS->STREAM_START recovery, worker exception => NO publish +
///        slot freed + worker survives (runtime_error AND bad_alloc),
///        cancellable debug gate (join returns; flush releases it),
///        stop drains ALL queued, restart reuses slots, old tasks never run.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "async/face_worker.hpp"
#include "test_helpers.hpp"

namespace fa = face_async;

// ---------------------------------------------------------------------------
// 7. FLUSH semantics
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, FlushReclaimsOnlyQueuedNotCopyingOrInferencing) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::uint32_t s0 = 0, s1 = 0;
  fa::Epoch ep0, ep1;
  ASSERT_TRUE(h.w->try_acquire_slot(s0, "cam-001", &ep0));  // COPYING
  ASSERT_TRUE(h.w->try_acquire_slot(s1, "cam-001", &ep1));  // COPYING
  // commit s1 -> QUEUED in mailbox; s0 stays COPYING
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s1, "cam-001", 1, ep1, h.clk())));
  // FLUSH: only the QUEUED s1 is reclaimed; COPYING s0 untouched
  h.w->flush_start_bump();
  EXPECT_EQ(h.w->pool_counters().queued, 0u);
  EXPECT_EQ(h.w->pool_counters().copying, 1u);
  EXPECT_EQ(h.w->total_queue_depth(), 0u);
  EXPECT_TRUE(h.w->invariant_ok());
  h.w->cancel_copy(s0);
  EXPECT_EQ(h.w->pool_counters().copying, 0u);
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, FlushInsideCopyWindowCancelsCommitExactly) {
  // The exact race the expert mandated: producer acquires the slot (epoch
  // captured), a FULL flush completes inside the "copy window", then the
  // copy finishes and commit is attempted — it MUST fail and cancel_copy,
  // the task must never enter the new generation.
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::uint32_t s = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s, "cam-001", &ep));  // COPYING begins
  auto t = MakeTaskFromEpoch(s, "cam-001", 1, ep, h.clk());

  // FULL flush inside the copy window (bump + clear + stop)
  const auto gens = h.w->flush_start_bump();
  h.store->reset_epoch(gens.first, gens.second);
  h.store->clear();
  h.w->flush_stop();

  // copy "finished": commit must compare the captured epoch vs current
  EXPECT_FALSE(h.w->commit_task(t));
  EXPECT_EQ(h.w->pool_counters().copying, 0u);
  EXPECT_EQ(h.w->pool_counters().queued, 0u);
  EXPECT_EQ(h.w->total_queue_depth(), 0u);
  EXPECT_TRUE(h.w->invariant_ok());
}

// ---------------------------------------------------------------------------
// 8. Five-flag accepting matrix + authority rules
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, AcceptingFiveFlagMatrix) {
  for (int mask = 0; mask < 32; ++mask) {
    WorkerHarness h;
    const bool running = (mask & 1) != 0;
    const bool ready = (mask & 2) != 0;
    const bool compat = (mask & 4) != 0;
    const bool flushing = (mask & 8) != 0;
    const bool eos = (mask & 16) != 0;
    h.w->set_running(running);
    h.w->set_processor_ready(ready);
    h.w->set_caps_compatible(compat);
    if (flushing) h.w->flush_start_bump();
    if (eos) h.w->set_eos();
    const bool expected = running && ready && compat && !flushing && !eos;
    EXPECT_EQ(h.w->accepting(), expected) << "mask=" << mask;
  }
}

TEST(FaceWorkerTest, FlushStopDoesNotRestoreCapsCompatible) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);
  EXPECT_TRUE(h.w->accepting());
  h.w->set_caps_compatible(false);
  EXPECT_FALSE(h.w->accepting());
  h.w->flush_start_bump();
  h.w->flush_stop();  // must NOT restore caps_compatible
  EXPECT_FALSE(h.w->accepting());
}

TEST(FaceWorkerTest, StreamStartDoesNotClearUnpairedFlushing) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);
  h.w->flush_start_bump();
  h.w->set_eos();
  h.w->stream_start_bump();  // clears eos only
  EXPECT_FALSE(h.w->accepting());
  h.w->flush_stop();
  EXPECT_TRUE(h.w->accepting());
}

TEST(FaceWorkerTest, ProducerAcquireBlockedByEachFlag) {
  WorkerHarness h;
  std::uint32_t s = 0;
  fa::Epoch ep;
  EXPECT_FALSE(h.w->try_acquire_slot(s, "cam-001", &ep));  // running=false
  h.w->set_running(true);
  EXPECT_FALSE(h.w->try_acquire_slot(s, "cam-001", &ep));  // processor not ready
  h.w->set_processor_ready(true);
  EXPECT_FALSE(h.w->try_acquire_slot(s, "cam-001", &ep));  // caps incompatible
  h.w->set_caps_compatible(true);
  EXPECT_TRUE(h.w->try_acquire_slot(s, "cam-001", &ep));
  h.w->cancel_copy(s);
  h.w->flush_start_bump();
  EXPECT_FALSE(h.w->try_acquire_slot(s, "cam-001", &ep));  // flushing
  h.w->flush_stop();
  h.w->set_eos();
  EXPECT_FALSE(h.w->try_acquire_slot(s, "cam-001", &ep));  // eos
}

// ---------------------------------------------------------------------------
// 13. EOS -> STREAM_START recovery
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, EosThenStreamStartResumesInference) {
  WorkerHarness h;
  std::atomic<int> inferred{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&inferred](const fa::TaskRecord&, void*) {
          inferred.fetch_add(1);
          return std::vector<fa::FaceBox>{Box(1, 1)};
        },
        nullptr);
  });

  std::uint32_t s = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s, "cam-001", &ep));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s, "cam-001", 1, ep, h.clk())));
  ASSERT_TRUE(h.wait_until_true([&] { return inferred.load() >= 1; },
                                std::chrono::seconds(2)));

  h.w->set_eos();
  std::uint32_t s2 = 0;
  EXPECT_FALSE(h.w->try_acquire_slot(s2, "cam-001", &ep));  // EOS blocks

  const auto gens = h.w->stream_start_bump();
  h.store->reset_epoch(gens.first, gens.second);
  fa::Epoch ep2;
  ASSERT_TRUE(h.w->try_acquire_slot(s2, "cam-001", &ep2));  // resumed
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s2, "cam-001", 2, ep2, h.clk())));
  ASSERT_TRUE(h.wait_until_true([&] { return inferred.load() >= 2; },
                                std::chrono::seconds(2)));

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

// ---------------------------------------------------------------------------
// 11. Worker exception handling (round-2: NO publish on failure)
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, InferenceExceptionSkipsPublishSlotFreedKeepsGoing) {
  WorkerHarness h;
  std::atomic<int> calls{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) -> std::vector<fa::FaceBox> {
          if (calls.fetch_add(1) == 0) throw std::runtime_error("graphExecute");
          return {Box(3, 3)};
        },
        nullptr);
  });

  std::uint32_t s0 = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s0, "cam-001", &ep));
  auto t0 = MakeTaskFromEpoch(s0, "cam-001", 1, ep, h.clk());
  ASSERT_TRUE(h.w->commit_task(t0));  // assigns t0.seq
  ASSERT_TRUE(h.wait_until_true([&] { return calls.load() >= 1; },
                                std::chrono::seconds(2)));
  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 0; },
      std::chrono::seconds(2)));

  // round-2 semantics: an exception publishes NOTHING — the old result
  // expires via TTL and the pipeline full-masks. Slot must be freed.
  EXPECT_EQ(h.store->latest("cam-001", kDefaultTtlUs), nullptr);

  // producer path stays OK: second task flows + is inferred (worker survived)
  std::uint32_t s1 = 0;
  fa::Epoch ep2;
  ASSERT_TRUE(h.w->try_acquire_slot(s1, "cam-001", &ep2));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s1, "cam-001", 2, ep2, h.clk())));
  ASSERT_TRUE(h.wait_until_true(
      [&] {
        auto r = h.store->latest("cam-001", kDefaultTtlUs);
        return r != nullptr;
      },
      std::chrono::seconds(2)));
  EXPECT_GE(h.w->counters().errors, 1u);

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, InferenceBadAllocSkipsPublishSlotFreedKeepsGoing) {
  WorkerHarness h;
  std::atomic<int> calls{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) -> std::vector<fa::FaceBox> {
          if (calls.fetch_add(1) == 0) throw std::bad_alloc();
          return {Box(3, 3)};
        },
        nullptr);
  });

  std::uint32_t s0 = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s0, "cam-001", &ep));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s0, "cam-001", 1, ep, h.clk())));
  ASSERT_TRUE(h.wait_until_true([&] { return calls.load() >= 1; },
                                std::chrono::seconds(2)));
  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 0; },
      std::chrono::seconds(2)));
  EXPECT_EQ(h.store->latest("cam-001", kDefaultTtlUs), nullptr);

  std::uint32_t s1 = 0;
  fa::Epoch ep2;
  ASSERT_TRUE(h.w->try_acquire_slot(s1, "cam-001", &ep2));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s1, "cam-001", 2, ep2, h.clk())));
  ASSERT_TRUE(h.wait_until_true([&] { return calls.load() >= 2; },
                                std::chrono::seconds(2)));
  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 0; },
      std::chrono::seconds(2)));

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, ProducerLatestReturnsNullWithoutWorkerAndNeverThrows) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);
  EXPECT_EQ(h.w->latest_result("cam-001"), nullptr);
  std::uint32_t s = 0;
  fa::Epoch ep;
  EXPECT_FALSE(h.w->try_acquire_slot(s, std::string(fa::kMaxCameraIdLen + 1, 'z'), &ep));
}

// ---------------------------------------------------------------------------
// 12. Cancellable debug gate: join returns; FLUSH releases it
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, GateIsCancellableSoJoinCanReturn) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);
  h.w->set_gate(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [](const fa::TaskRecord&, void*) {
          return std::vector<fa::FaceBox>{Box(4, 4)};
        },
        nullptr);
  });

  std::uint32_t s = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s, "cam-001", &ep));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s, "cam-001", 1, ep, h.clk())));
  // worker pops the task (slot -> INFERENCING) and parks in the gate
  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 1; },
      std::chrono::seconds(2)));

  // stop() releases the gate: join MUST return within the bounded wait
  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, GateReleasedByFlushRunsInference) {
  WorkerHarness h;
  std::atomic<int> entered{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);
  h.w->set_gate(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) {
          entered.fetch_add(1);
          return std::vector<fa::FaceBox>{Box(4, 4)};
        },
        nullptr);
  });

  std::uint32_t s = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s, "cam-001", &ep));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s, "cam-001", 1, ep, h.clk())));
  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 1; },
      std::chrono::seconds(2)));
  EXPECT_EQ(entered.load(), 0);  // parked in the gate, inference not run

  // FLUSH releases the gate (test-build semantics): inference runs
  h.w->flush_start_bump();
  h.w->flush_stop();
  ASSERT_TRUE(h.wait_until_true([&] { return entered.load() >= 1; },
                                std::chrono::seconds(2)));

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

// ---------------------------------------------------------------------------
// Round-2: stop drains ALL queued; restart reuses slots; old tasks never run
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, StopDrainsQueuedThenRestartReusesSlotsCleanly) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  // queue a task with NO worker running
  std::uint32_t s0 = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s0, "cam-001", &ep));
  auto t0 = MakeTaskFromEpoch(s0, "cam-001", 1, ep, h.clk());
  ASSERT_TRUE(h.w->commit_task(t0));
  EXPECT_EQ(h.w->total_queue_depth(), 1u);
  EXPECT_EQ(h.w->pool_counters().queued, 1u);

  // stop must drain the queued task and free the slot
  h.w->stop();
  EXPECT_EQ(h.w->total_queue_depth(), 0u);
  EXPECT_EQ(h.w->pool_counters().queued, 0u);
  EXPECT_EQ(h.w->pool_counters().free_n, h.w->pool_counters().allocated);
  EXPECT_TRUE(h.w->invariant_ok());

  // restart simulation on the same core (the plugin recreates the worker
  // object per start; pure-logic restart = re-enable the flags)
  h.w->set_running(true);
  std::atomic<int> seen{0};
  std::atomic<std::uint64_t> last_det{0};
  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord& t, void*) {
          seen.fetch_add(1);
          last_det.store(t.det_frame_id);
          return std::vector<fa::FaceBox>{Box(1, 1)};
        },
        nullptr);
  });

  std::uint32_t s1 = 0;
  fa::Epoch ep2;
  ASSERT_TRUE(h.w->try_acquire_slot(s1, "cam-001", &ep2));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s1, "cam-001", 2, ep2, h.clk())));
  ASSERT_TRUE(h.wait_until_true([&] { return seen.load() >= 1; },
                                std::chrono::seconds(2)));
  EXPECT_EQ(last_det.load(), 2u);  // the OLD task (det=1) never ran
  EXPECT_EQ(seen.load(), 1);       // exactly one task ran

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

// ---------------------------------------------------------------------------
// Result publish round-trip through the worker (VALID_FACES / VALID_EMPTY)
// ---------------------------------------------------------------------------
TEST(FaceWorkerTest, ValidFacesAndValidEmptyPublished) {
  WorkerHarness h;
  std::atomic<int> calls{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord& t, void*) {
          calls.fetch_add(1);
          if (t.det_frame_id == 1) return std::vector<fa::FaceBox>{Box(1, 1)};
          return std::vector<fa::FaceBox>{};
        },
        nullptr);
  });

  std::uint32_t s0 = 0;
  fa::Epoch ep;
  ASSERT_TRUE(h.w->try_acquire_slot(s0, "cam-001", &ep));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s0, "cam-001", 1, ep, h.clk())));
  std::shared_ptr<const fa::FaceResultEntry> r;
  ASSERT_TRUE(h.wait_until_true(
      [&] {
        r = h.store->latest("cam-001", kDefaultTtlUs);
        return r != nullptr;
      },
      std::chrono::seconds(2)));
  EXPECT_EQ(r->status, fa::ResultStatus::VALID_FACES);
  EXPECT_EQ(r->faces.size(), 1u);
  EXPECT_EQ(r->det_frame_id, 1u);
  EXPECT_EQ(r->sample_mono_us, 0);

  std::uint32_t s1 = 0;
  fa::Epoch ep2;
  ASSERT_TRUE(h.w->try_acquire_slot(s1, "cam-001", &ep2));
  ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s1, "cam-001", 2, ep2, h.clk())));
  ASSERT_TRUE(h.wait_until_true(
      [&] {
        auto r2 = h.store->latest("cam-001", kDefaultTtlUs);
        return r2 && r2->status == fa::ResultStatus::VALID_EMPTY;
      },
      std::chrono::seconds(2)));

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}
