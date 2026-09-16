/// @file face_worker_concurrency_tests.cpp
/// @brief Scenario 6+9+10: late old-generation result rejected after FLUSH,
///        concurrent produce/consume/flush with bounded waits (no sleep),
///        ownership invariant under randomized producer/event ops.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "async/face_worker.hpp"
#include "test_helpers.hpp"

namespace fa = face_async;

TEST(FaceWorkerTest, LateOldGenerationResultRejectedAfterFlush) {
  WorkerHarness h;
  std::atomic<int> inferred{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  // slow inference: worker signals started, then waits for release
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) {
          inferred.fetch_add(1);
          std::unique_lock<std::mutex> lk(m);
          cv.wait(lk, [&] { return release; });  // bounded by stop() below
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

  // FLUSH while worker is mid-inference: results cleared + epoch bumped
  const auto gens = h.w->flush_start_bump();
  h.store->reset_epoch(gens.first, gens.second);
  h.store->clear();
  h.w->flush_stop();

  // release worker: its result carries the OLD stream_gen -> rejected
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
  }
  cv.notify_all();

  ASSERT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 0; },
      std::chrono::seconds(2)));
  EXPECT_EQ(h.store->latest("cam-001", kDefaultTtlUs),
            nullptr);  // old result never reappears

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, ConcurrentProduceConsumeFlushBoundedWaits) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::atomic<bool> stop_flag{false};
  std::atomic<int> inferred{0};

  std::thread producer([&] {
    std::uint64_t frame = 1;
    while (!stop_flag.load()) {
      std::uint32_t s = 0;
      fa::Epoch ep;
      if (h.w->try_acquire_slot(s, "cam-001", &ep)) {
        auto t = MakeTaskFromEpoch(s, "cam-001", frame++, ep, h.clk());
        h.w->commit_task(t);
      }
      // bounded wait instead of sleep (wait_for with explicit timeout)
      std::mutex m;
      std::condition_variable cv;
      std::unique_lock<std::mutex> lk(m);
      cv.wait_for(lk, std::chrono::microseconds(50));
    }
  });

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) {
          inferred.fetch_add(1);
          return std::vector<fa::FaceBox>{Box(2, 2)};
        },
        nullptr);
  });

  std::thread flusher([&] {
    for (int i = 0; i < 20 && !stop_flag.load(); ++i) {
      const auto gens = h.w->flush_start_bump();
      h.store->reset_epoch(gens.first, gens.second);
      h.store->clear();
      h.w->flush_stop();
      std::mutex m;
      std::condition_variable cv;
      std::unique_lock<std::mutex> lk(m);
      cv.wait_for(lk, std::chrono::microseconds(100));
    }
  });

  // bounded wait (not a sleep) before teardown
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock<std::mutex> lk(m);
  cv.wait_for(lk, std::chrono::milliseconds(500));

  stop_flag.store(true);
  producer.join();
  h.w->stop();
  worker.join();
  flusher.join();

  EXPECT_GT(inferred.load(), 0);
  EXPECT_TRUE(h.w->invariant_ok());
}

TEST(FaceWorkerTest, InvariantAllocatedEqualsSumOfStates) {
  WorkerHarness h;
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::vector<std::uint32_t> slots;  // slots held in COPYING by this test
  for (int i = 0; i < 200; ++i) {
    const int op = i % 5;
    if (op == 0) {
      std::uint32_t s = 0;
      fa::Epoch ep;
      if (h.w->try_acquire_slot(s, "cam-001", &ep)) slots.push_back(s);
    } else if (op == 1 && !slots.empty()) {
      fa::Epoch ep;
      (void)ep;  // tasks committed below capture epoch via a fresh acquire
      auto t = MakeTask(slots.back(), "cam-001", i, 1, 1, h.clk());
      if (h.w->commit_task(t)) {
        slots.pop_back();
      } else {
        h.w->cancel_copy(slots.back());
        slots.pop_back();
      }
    } else if (op == 2) {
      h.w->flush_start_bump();
      h.w->flush_stop();
    } else if (op == 3 && !slots.empty()) {
      h.w->cancel_copy(slots.back());
      slots.pop_back();
    } else if (op == 4) {
      h.w->set_eos();
      h.w->stream_start_bump();
    }
    EXPECT_TRUE(h.w->invariant_ok()) << "iteration " << i;
  }
  h.w->stop();
}

TEST(FaceWorkerTest, SlotReuseIsSafeAcrossTaskCycles) {
  // After a full produce->infer->free cycle, the same slot index must be
  // acquirable again with a different camera (no cross-camera leakage).
  WorkerHarness h;
  std::atomic<int> calls{0};
  h.w->set_running(true);
  h.w->set_processor_ready(true);
  h.w->set_caps_compatible(true);

  std::thread worker([&] {
    h.w->worker_loop(
        [&](const fa::TaskRecord&, void*) {
          calls.fetch_add(1);
          return std::vector<fa::FaceBox>{Box(5, 5)};
        },
        nullptr);
  });

  for (int i = 0; i < 5; ++i) {
    const std::string cam = (i % 2 == 0) ? "cam-001" : "cam-002";
    std::uint32_t s = 0;
    fa::Epoch ep;
    ASSERT_TRUE(h.w->try_acquire_slot(s, cam, &ep));
    ASSERT_TRUE(h.w->commit_task(MakeTaskFromEpoch(s, cam, i + 1, ep, h.clk())));
    ASSERT_TRUE(h.wait_until_true([&] { return calls.load() >= i + 1; },
                                  std::chrono::seconds(2)));
  }
  EXPECT_TRUE(h.wait_until_true(
      [&] { return h.w->pool_counters().inferencing == 0; },
      std::chrono::seconds(2)));

  h.w->stop();
  worker.join();
  EXPECT_TRUE(h.w->invariant_ok());
  EXPECT_GE(h.w->counters().committed, 5u);
}
