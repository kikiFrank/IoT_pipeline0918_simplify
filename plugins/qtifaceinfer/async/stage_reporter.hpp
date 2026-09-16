/// @file stage_reporter.hpp
/// @brief Per-stage enter/exit counters + current-stage tracking (pure logic).
///
/// Used for [faceinfer] hb diagnostics: worker=<STAGE> stuck_ms=<n>.
/// Diagnostic ONLY — no watchdog, no auto-recovery (user decision).
/// Thread-safe via atomics; clock injected for tests.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

namespace face_async {

enum class FaceStage : std::uint32_t {
  IDLE = 0,
  COPY_INPUT,
  OPENCL_IMPORT,
  CL_ENQUEUE,
  CL_FINISH,
  DMA_SYNC,
  QNN_COPY,
  GRAPH_EXECUTE,
  DEQUANT,
  SCRFD_DECODE,
  PUBLISH_RESULT,
  kCount  // sentinel
};

constexpr std::uint32_t kFaceStageCount =
    static_cast<std::uint32_t>(FaceStage::kCount);

struct StageStats {
  std::uint64_t enter_count = 0;
  std::uint64_t exit_count = 0;
  std::uint64_t total_us = 0;
};

class StageReporter {
 public:
  explicit StageReporter(std::function<std::int64_t()> now_us)
      : now_us_(std::move(now_us)) {}

  void enter(FaceStage s) {
    const auto idx = static_cast<std::uint32_t>(s);
    stages_[idx].enters.fetch_add(1, std::memory_order_relaxed);
    enter_us_.store(now_us_(), std::memory_order_relaxed);
    current_.store(idx, std::memory_order_relaxed);
  }

  void leave(FaceStage s) {
    const auto idx = static_cast<std::uint32_t>(s);
    const std::uint64_t e = stages_[idx].enters.load(std::memory_order_relaxed);
    if (e == 0) return;  // leave without enter: harmless no-op
    const std::int64_t started = enter_us_.load(std::memory_order_relaxed);
    if (started > 0) {
      const std::int64_t dur = now_us_() - started;
      if (dur > 0)
        stages_[idx].total_us.fetch_add(static_cast<std::uint64_t>(dur),
                                        std::memory_order_relaxed);
    }
    stages_[idx].exits.fetch_add(1, std::memory_order_relaxed);
    if (current_.load(std::memory_order_relaxed) == idx)
      current_.store(static_cast<std::uint32_t>(FaceStage::IDLE),
                     std::memory_order_relaxed);
  }

  FaceStage current() const {
    return static_cast<FaceStage>(current_.load(std::memory_order_relaxed));
  }

  std::int64_t current_enter_us() const {
    return enter_us_.load(std::memory_order_relaxed);
  }

  StageStats stats(FaceStage s) const {
    const auto idx = static_cast<std::uint32_t>(s);
    StageStats out;
    out.enter_count = stages_[idx].enters.load(std::memory_order_relaxed);
    out.exit_count = stages_[idx].exits.load(std::memory_order_relaxed);
    out.total_us = stages_[idx].total_us.load(std::memory_order_relaxed);
    return out;
  }

 private:
  struct S {
    std::atomic<std::uint64_t> enters{0};
    std::atomic<std::uint64_t> exits{0};
    std::atomic<std::uint64_t> total_us{0};
  };
  std::array<S, kFaceStageCount> stages_{};
  std::atomic<std::uint32_t> current_{0};
  std::atomic<std::int64_t> enter_us_{0};
  std::function<std::int64_t()> now_us_;
};

}  // namespace face_async
