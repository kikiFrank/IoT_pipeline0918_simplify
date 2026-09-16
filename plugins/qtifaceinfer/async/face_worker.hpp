/// @file face_worker.hpp
/// @brief Async core: producer fast-path + single inference worker loop
///        (pure logic, thread-safe).
///
/// GENERATIONS LIVE INSIDE THIS CLASS (single state mutex):
///   caps_gen_ / stream_gen_ are owned by the state mutex — the plugin MUST
///   NOT read/write generation counters itself. try_acquire_slot returns the
///   captured epoch; commit_task compares the task's captured epoch against
///   the CURRENT epoch under the lock. A FLUSH that completes fully inside
///   the copy window therefore always rejects the commit (cancel_copy).
///
/// Five-flag accepting state machine (authority rules, no cross-restore):
///   running / processor_ready / caps_compatible / flushing / eos
///   accepting = running && processor_ready && caps_compatible && !flushing && !eos
///   - flush_start_bump(): flushing=true, ++stream_gen_, drains QUEUED slots
///   - flush_stop(): clears flushing ONLY
///   - set_eos(): sets eos ONLY (+drains QUEUED slots)
///   - stream_start_bump(): clears eos ONLY, ++stream_gen_
///   - caps_compatible is restored ONLY by the plugin (normal stop/re-init)
///
/// stop(): running=false, releases the debug gate, drains ALL QUEUED slots
/// (evict) under the lock — nothing queued may survive a stop; slot numbers
/// are free for the next cycle and old tasks can never evict new ones.
///
/// Worker loop: condition_variable waits only (never sleep/poll). An RAII
/// scope guard guarantees complete_inference for the INFERENCING slot on
/// EVERY exit path (exceptions included); a whole-loop try/catch is the
/// final boundary so no C++ exception can cross the GstTask C callback.
/// Inference/entry-build/publish exceptions publish nothing (old results
/// expire via TTL -> full mask), count an error, and the worker continues.
///
/// Locking: ONE mutex protects pool+mailbox+flags+generations. The
/// ResultStore has its own mutex. The two locks are NEVER nested.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "face_mailbox.hpp"
#include "face_async_types.hpp"
#include "result_store.hpp"
#include "slot_pool.hpp"
#include "stage_reporter.hpp"

namespace face_async {

using InferFn = std::function<std::vector<FaceBox>(const TaskRecord& task, void* userdata)>;

/// Epoch captured by the producer at slot acquire time.
struct Epoch {
  std::uint64_t caps_gen = 0;
  std::uint64_t stream_gen = 0;
};

struct WorkerCounters {
  std::uint64_t produced = 0;
  std::uint64_t committed = 0;
  std::uint64_t rejected_commit = 0;
  std::uint64_t cancelled_copy = 0;
  std::uint64_t inferences = 0;
  std::uint64_t errors = 0;
};

class FaceWorker {
 public:
  FaceWorker(std::uint32_t pool_size, std::shared_ptr<ResultStore> store,
             std::function<std::int64_t()> now_us, std::int64_t ttl_us)
      : pool_(pool_size), store_(std::move(store)), now_us_(std::move(now_us)),
        ttl_us_(ttl_us) {}

  std::uint32_t pool_size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pool_.size();
  }

  // ------------------------------------------------------------------
  // Producer (streaming thread) — never blocks on inference
  // ------------------------------------------------------------------

  /// FREE -> COPYING. Refused when !accepting or camera over cap/id too long
  /// (camera validated BEFORE any slot is touched). Never waits.
  /// On success the captured epoch is returned in `out` — the producer MUST
  /// store it into the TaskRecord for the later commit comparison.
  bool try_acquire_slot(std::uint32_t& idx, const std::string& camera_id,
                        Epoch* out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!accepting_locked()) return false;
    if (!mailbox_.camera_allowed(camera_id)) return false;
    if (!pool_.acquire(idx)) return false;
    if (out) {
      out->caps_gen = caps_gen_;
      out->stream_gen = stream_gen_;
    }
    ++counters_.produced;
    return true;
  }

  /// Copy finished: re-check accepting + the TASK's CAPTURED epoch vs the
  /// current epoch + slot state, then COPYING -> QUEUED + mailbox push.
  /// Any mismatch => cancel_copy (slot back to FREE, task abandoned).
  /// Push failure => slot rolled back (no orphaned QUEUED slot).
  /// Assigns the task's seq (monotonic, producer-owned) — the caller does
  /// not need the seq back, so the task is taken BY VALUE.
  bool commit_task(TaskRecord task) {
    std::lock_guard<std::mutex> lk(mtx_);
    task.seq = ++seq_ctr_;
    const bool ok = accepting_locked() &&
                    task.caps_gen == caps_gen_ &&
                    task.stream_gen == stream_gen_ &&
                    pool_.state(task.slot_idx) == SlotState::COPYING;
    if (!ok) {
      pool_.cancel_copy(task.slot_idx);
      ++counters_.rejected_commit;
      return false;
    }
    pool_.commit_copy(task.slot_idx);
    TaskRecord evicted;
    const PushResult r = mailbox_.push(task, &evicted);
    if (r == PushResult::REJECTED) {
      pool_.evict(task.slot_idx);  // QUEUED -> FREE rollback
      ++counters_.rejected_commit;
      return false;
    }
    if (r == PushResult::EVICTED_OLD) pool_.evict(evicted.slot_idx);
    ++counters_.committed;
    cv_.notify_all();
    return true;
  }

  /// Copy/layout failure path: COPYING -> FREE.
  void cancel_copy(std::uint32_t idx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pool_.cancel_copy(idx)) ++counters_.cancelled_copy;
  }

  /// Latest valid result snapshot (ResultStore lock, no nesting).
  std::shared_ptr<const FaceResultEntry> latest_result(
      const std::string& cam, std::int64_t* age_out = nullptr) {
    return store_->latest(cam, ttl_us_, age_out);
  }

  // ------------------------------------------------------------------
  // Events / lifecycle (any thread)
  // ------------------------------------------------------------------

  void set_running(bool v) {
    std::lock_guard<std::mutex> lk(mtx_);
    running_ = v;
    cv_.notify_all();
  }
  void set_processor_ready(bool v) {
    std::lock_guard<std::mutex> lk(mtx_);
    processor_ready_ = v;
    cv_.notify_all();
  }
  void set_caps_compatible(bool v) {
    std::lock_guard<std::mutex> lk(mtx_);
    caps_compatible_ = v;
    cv_.notify_all();
  }

  /// caps change (set_caps): ++caps_gen_ AND ++stream_gen_ atomically.
  /// Returns the new generation pair (for the store's reset_epoch).
  std::pair<std::uint64_t, std::uint64_t> bump_caps_and_stream_gen() {
    std::lock_guard<std::mutex> lk(mtx_);
    ++caps_gen_;
    ++stream_gen_;
    return {caps_gen_, stream_gen_};
  }

  /// FLUSH_START: flushing=true, ++stream_gen_, drains QUEUED slots (evict);
  /// COPYING and INFERENCING are never touched. Never waits for the worker.
  /// Releases the cancellable debug gate (test builds only).
  /// Returns the new generation pair.
  std::pair<std::uint64_t, std::uint64_t> flush_start_bump() {
    std::lock_guard<std::mutex> lk(mtx_);
    flushing_ = true;
    ++stream_gen_;
    gate_ = false;  // a gated worker must always be releasable by FLUSH
    drain_queued_locked();
    cv_.notify_all();
    gate_cv_.notify_all();
    return {caps_gen_, stream_gen_};
  }

  /// FLUSH_STOP: clears flushing ONLY.
  void flush_stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    flushing_ = false;
    cv_.notify_all();
  }

  /// EOS: sets eos ONLY (no generation bump). Drains QUEUED slots; never
  /// waits for in-flight inference.
  void set_eos() {
    std::lock_guard<std::mutex> lk(mtx_);
    eos_ = true;
    drain_queued_locked();
    cv_.notify_all();
  }

  /// STREAM_START: clears eos ONLY, ++stream_gen_. Never clears unpaired
  /// flushing. Returns the new generation pair.
  std::pair<std::uint64_t, std::uint64_t> stream_start_bump() {
    std::lock_guard<std::mutex> lk(mtx_);
    eos_ = false;
    ++stream_gen_;
    cv_.notify_all();
    return {caps_gen_, stream_gen_};
  }

  /// stop: running=false, releases the debug gate, DRAINS ALL QUEUED slots
  /// (evict) so nothing queued survives into a later cycle; the worker loop
  /// returns on the next wake-up.
  void stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    running_ = false;
    gate_ = false;
    drain_queued_locked();
    cv_.notify_all();
    gate_cv_.notify_all();
  }

  bool accepting() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return accepting_locked();
  }

  /// Cancellable debug gate (test builds only). Released by stop() AND by
  /// flush_start_bump() — a gated worker must always be releasable.
  void set_gate(bool v) {
    std::lock_guard<std::mutex> lk(mtx_);
    gate_ = v;
    if (!v) gate_cv_.notify_all();
  }

  /// Test-build post-inference delay (bounded, cancellable wait — not a
  /// plain sleep). 0 disables.
  void set_test_delay_ms(int ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    test_delay_us_ = ms > 0 ? static_cast<std::int64_t>(ms) * 1000 : 0;
  }

  /// Optional diagnostic stage reporter (PUBLISH_RESULT hooks; thread-safe).
  void set_stage_reporter(std::shared_ptr<StageReporter> sr) {
    std::lock_guard<std::mutex> lk(mtx_);
    stage_reporter_ = std::move(sr);
  }

  /// stop-path verification: true only when EVERY slot is FREE (worker
  /// joined). Refuses (keeps states) when anything is still
  /// COPYING/QUEUED/INFERENCING — never forces states.
  bool verify_idle_and_reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    return pool_.reset_all();
  }

  /// All camera ids ever seen — for heartbeat summaries.
  std::vector<std::string> known_cameras() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mailbox_.known_cameras();
  }

  // ------------------------------------------------------------------
  // Worker (single GstTask thread)
  // ------------------------------------------------------------------

  /// RAII guard: guarantees complete_inference for the popped slot on every
  /// exit path, exceptions included — the slot can never leak.
  class InferenceSlotGuard {
   public:
    InferenceSlotGuard(FaceWorker* w, std::uint32_t slot) : w_(w), slot_(slot) {}
    InferenceSlotGuard(const InferenceSlotGuard&) = delete;
    InferenceSlotGuard& operator=(const InferenceSlotGuard&) = delete;
    ~InferenceSlotGuard() {
      if (w_) {
        std::lock_guard<std::mutex> lk(w_->mtx_);
        if (w_->pool_.state(slot_) == SlotState::INFERENCING)
          w_->pool_.complete_inference(slot_);
        w_->cv_.notify_all();
      }
    }
    void disarm() { w_ = nullptr; }

   private:
    FaceWorker* w_;
    std::uint32_t slot_;
  };

  void worker_loop(InferFn infer, void* userdata) {
    try {
      for (;;) {
        TaskRecord task;
        bool have = false;
        {
          std::unique_lock<std::mutex> lk(mtx_);
          cv_.wait(lk, [&] {
            return !running_ ||
                   (accepting_locked() && mailbox_.total_depth() > 0);
          });
          if (!running_) return;
          auto t = mailbox_.pop_round_robin();
          if (t && pool_.begin_inference(t->slot_idx)) {
            task = *t;
            have = true;
          }
        }
        if (!have) continue;

        // Cancellable debug gate: released by stop()/set_gate(false)/flush.
        {
          std::unique_lock<std::mutex> lk(mtx_);
          gate_cv_.wait(lk, [&] { return !gate_ || !running_; });
          if (!running_) {
            pool_.complete_inference(task.slot_idx);  // never leak the slot
            return;
          }
        }

        // From here on, the slot returns to FREE on EVERY path.
        InferenceSlotGuard slot_guard(this, task.slot_idx);

        // Inference: all blocking vendor calls happen HERE, never under a lock.
        std::vector<FaceBox> boxes;
        ResultStatus st = ResultStatus::VALID_FACES;
        try {
          boxes = infer(task, userdata);
          st = boxes.empty() ? ResultStatus::VALID_EMPTY
                             : ResultStatus::VALID_FACES;
        } catch (...) {
          // no publish on any inference/entry-build/publish failure: the old
          // result expires via TTL -> full mask. Worker keeps going.
          st = ResultStatus::UNAVAILABLE;
          boxes.clear();
          std::lock_guard<std::mutex> lk(mtx_);
          ++counters_.errors;
        }

        if (st != ResultStatus::UNAVAILABLE) {
          auto entry = std::make_shared<FaceResultEntry>();
          entry->status = st;
          entry->faces = std::move(boxes);
          entry->det_frame_id = task.det_frame_id;
          entry->pts_ns = task.pts_ns;
          entry->caps_gen = task.caps_gen;
          entry->stream_gen = task.stream_gen;
          entry->seq = task.seq;  // task's OWN seq — never fabricated
          entry->sample_mono_us = task.sample_mono_us;
          entry->width = task.width;
          entry->height = task.height;

          // Atomic publish: epoch+seq+TTL validated inside the store lock.
          if (stage_reporter_) stage_reporter_->enter(FaceStage::PUBLISH_RESULT);
          store_->publish_if_current(task.camera_id, entry, ttl_us_);
          if (stage_reporter_) stage_reporter_->leave(FaceStage::PUBLISH_RESULT);
        }

        {
          std::lock_guard<std::mutex> lk(mtx_);
          ++counters_.inferences;
        }
        (void)slot_guard;  // complete_inference on scope exit

        // Test-build post-inference delay: bounded cancellable wait.
        if (test_delay_us_ > 0) {
          std::unique_lock<std::mutex> lk(mtx_);
          cv_.wait_for(lk, std::chrono::microseconds(test_delay_us_),
                       [&] { return !running_; });
        }
      }
    } catch (...) {
      // ABSOLUTE boundary: no C++ exception may cross the GstTask C callback.
      std::lock_guard<std::mutex> lk(mtx_);
      ++counters_.errors;
    }
  }

  // ------------------------------------------------------------------
  // Stats
  // ------------------------------------------------------------------

  SlotCounters pool_counters() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pool_.counters();
  }
  MailboxCounters mailbox_counters() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mailbox_.counters();
  }
  WorkerCounters counters() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return counters_;
  }
  std::size_t queue_depth(const std::string& cam) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mailbox_.depth(cam);
  }
  std::size_t total_queue_depth() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return mailbox_.total_depth();
  }
  bool invariant_ok() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pool_.invariant_ok();
  }

 private:
  bool accepting_locked() const {
    return running_ && processor_ready_ && caps_compatible_ && !flushing_ &&
           !eos_;
  }

  void drain_queued_locked() {
    auto drained = mailbox_.drain_queued();
    for (const auto& t : drained) pool_.evict(t.slot_idx);
  }

  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::condition_variable gate_cv_;
  bool running_ = false;
  bool processor_ready_ = false;
  bool caps_compatible_ = false;
  bool flushing_ = false;
  bool eos_ = false;
  bool gate_ = false;
  std::uint64_t caps_gen_ = 0;
  std::uint64_t stream_gen_ = 0;
  std::uint64_t seq_ctr_ = 0;
  std::int64_t test_delay_us_ = 0;

  SlotPool pool_;
  FaceMailbox mailbox_;
  std::shared_ptr<ResultStore> store_;
  std::shared_ptr<StageReporter> stage_reporter_;
  std::function<std::int64_t()> now_us_;  // reserved for future per-op stamps
  std::int64_t ttl_us_ = 0;
  WorkerCounters counters_{};
};

}  // namespace face_async
