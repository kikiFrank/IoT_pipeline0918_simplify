/// @file test_helpers.hpp
/// @brief Shared helpers for face_async pure-logic tests (fake clock, task/entry builders)
///
/// Concurrency rule: NO std::this_thread::sleep_for anywhere in the suite;
/// only condition_variable bounded waits (wait_for with explicit timeout).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "async/face_async_types.hpp"
#include "async/face_worker.hpp"
#include "async/result_store.hpp"

namespace fa = face_async;

// ---------------------------------------------------------------------------
// Fake monotonic clock (microseconds), shareable across threads via atomic.
// ---------------------------------------------------------------------------
struct FakeClock {
  std::atomic<std::int64_t> us{0};
  std::int64_t operator()() { return us.load(std::memory_order_relaxed); }
  void advance(std::int64_t d) { us.fetch_add(d, std::memory_order_relaxed); }
};

inline std::shared_ptr<fa::FaceResultEntry> MakeEntry(
    fa::ResultStatus st, std::uint64_t det_frame, std::uint64_t caps_gen,
    std::uint64_t stream_gen, std::uint64_t seq, std::int64_t sample_us,
    std::uint32_t w = 640, std::uint32_t h = 360) {
  auto e = std::make_shared<fa::FaceResultEntry>();
  e->status = st;
  e->det_frame_id = det_frame;
  e->caps_gen = caps_gen;
  e->stream_gen = stream_gen;
  e->seq = seq;
  e->sample_mono_us = sample_us;
  e->width = w;
  e->height = h;
  return e;
}

inline fa::TaskRecord MakeTask(std::uint32_t slot, const std::string& cam,
                               std::uint64_t det_frame, std::uint64_t caps_gen,
                               std::uint64_t stream_gen, std::int64_t sample_us) {
  fa::TaskRecord t;
  t.slot_idx = slot;
  t.camera_id = cam;
  t.det_frame_id = det_frame;
  t.pts_ns = det_frame * 40'000'000ULL;  // 25fps
  t.caps_gen = caps_gen;
  t.stream_gen = stream_gen;
  t.sample_mono_us = sample_us;
  t.width = 640;
  t.height = 360;
  return t;
}

inline fa::FaceBox Box(float x1, float y1) {
  fa::FaceBox b;
  b.x1 = x1;
  b.y1 = y1;
  b.x2 = x1 + 10;
  b.y2 = y1 + 10;
  b.score = 0.9f;
  return b;
}

inline fa::TaskRecord MakeTaskFromEpoch(std::uint32_t slot,
                                        const std::string& cam,
                                        std::uint64_t det_frame,
                                        const fa::Epoch& ep,
                                        std::int64_t sample_us) {
  return MakeTask(slot, cam, det_frame, ep.caps_gen, ep.stream_gen, sample_us);
}

// TTL used by most worker tests: 250ms in microseconds.
constexpr std::int64_t kDefaultTtlUs = 250'000;

struct WorkerHarness {
  FakeClock clk;
  std::shared_ptr<fa::ResultStore> store =
      std::make_shared<fa::ResultStore>([this] { return clk(); });
  std::unique_ptr<fa::FaceWorker> w = std::make_unique<fa::FaceWorker>(
      4, store, [this] { return clk(); }, kDefaultTtlUs);

  WorkerHarness() {
    // mirror the plugin: generations live in the worker; the store epoch
    // follows them (initial caps bump => (1,1))
    const auto gens = w->bump_caps_and_stream_gen();
    store->reset_epoch(gens.first, gens.second);
  }

  // Bounded spin-wait on a predicate (condition-variable-free variant of a
  // bounded wait; never uses sleep).
  template <typename Pred>
  bool wait_until_true(Pred pred, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return pred();
  }
};
