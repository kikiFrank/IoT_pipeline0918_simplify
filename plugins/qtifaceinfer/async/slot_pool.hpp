/// @file slot_pool.hpp
/// @brief Strict slot state machine (pure logic, NO OS resources).
///
/// States: FREE -> COPYING -> QUEUED -> INFERENCING -> FREE
///
/// Strict transitions only — no generic release:
///   acquire           FREE        -> COPYING
///   commit_copy       COPYING     -> QUEUED
///   cancel_copy       COPYING     -> FREE
///   evict             QUEUED      -> FREE
///   begin_inference   QUEUED      -> INFERENCING
///   complete_inference INFERENCING-> FREE
///
/// Any illegal transition is rejected, KEEPS the original state, and bumps
/// n_illegal. INFERENCING is NEVER forced back to FREE to "restore counters".
/// reset_all() refuses while any slot is INFERENCING (may still be touched
/// by the worker); it is only allowed after the worker has been joined.
///
/// NOT thread-safe by itself: standalone (single-thread tests) or composed
/// under the caller's lock (FaceWorker uses it under its mutex).

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace face_async {

enum class SlotState : std::uint8_t { FREE, COPYING, QUEUED, INFERENCING };

struct SlotCounters {
  std::uint64_t allocated = 0;
  std::uint64_t free_n = 0;
  std::uint64_t copying = 0;
  std::uint64_t queued = 0;
  std::uint64_t inferencing = 0;
  std::uint64_t evicted = 0;
  std::uint64_t noslot = 0;
  std::uint64_t illegal = 0;
};

class SlotPool {
 public:
  explicit SlotPool(std::uint32_t n) : states_(n, SlotState::FREE) {
    counters_.allocated = n;
    counters_.free_n = n;
  }

  /// FREE -> COPYING. Returns false immediately when no FREE slot exists
  /// (never blocks, never waits).
  bool acquire(std::uint32_t& idx) {
    for (std::uint32_t i = 0; i < states_.size(); ++i) {
      if (states_[i] == SlotState::FREE) {
        set_state(i, SlotState::COPYING);
        idx = i;
        return true;
      }
    }
    ++counters_.noslot;
    return false;
  }

  bool commit_copy(std::uint32_t idx) { return transition(idx, SlotState::COPYING, SlotState::QUEUED); }
  bool cancel_copy(std::uint32_t idx) { return transition(idx, SlotState::COPYING, SlotState::FREE); }
  bool begin_inference(std::uint32_t idx) { return transition(idx, SlotState::QUEUED, SlotState::INFERENCING); }
  bool complete_inference(std::uint32_t idx) { return transition(idx, SlotState::INFERENCING, SlotState::FREE); }

  /// QUEUED -> FREE (mailbox eviction / FLUSH reclaim / push-failure rollback).
  bool evict(std::uint32_t idx) {
    if (!transition(idx, SlotState::QUEUED, SlotState::FREE)) return false;
    ++counters_.evicted;
    return true;
  }

  /// All -> FREE. Only allowed when EVERY slot is ALREADY FREE (worker
  /// joined AND queues drained). Refuses when ANY slot is still
  /// COPYING/QUEUED/INFERENCING — never forces a state back to FREE.
  bool reset_all() {
    for (const auto s : states_) {
      if (s != SlotState::FREE) {
        ++counters_.illegal;
        return false;
      }
    }
    recount();
    return true;
  }

  SlotState state(std::uint32_t idx) const {
    if (idx >= states_.size()) return SlotState::FREE;
    return states_[idx];
  }

  std::uint32_t size() const { return static_cast<std::uint32_t>(states_.size()); }
  const SlotCounters& counters() const { return counters_; }

  /// Ownership invariant: allocated == free+copying+queued+inferencing.
  bool invariant_ok() const {
    return counters_.allocated ==
           counters_.free_n + counters_.copying + counters_.queued +
               counters_.inferencing;
  }

 private:
  bool valid(std::uint32_t idx) const { return idx < states_.size(); }

  bool transition(std::uint32_t idx, SlotState from, SlotState to) {
    if (!valid(idx) || states_[idx] != from) {
      ++counters_.illegal;  // state KEPT as-is
      return false;
    }
    set_state(idx, to);
    return true;
  }

  void set_state(std::uint32_t idx, SlotState to) {
    adjust(states_[idx], -1);
    states_[idx] = to;
    adjust(to, +1);
  }

  void adjust(SlotState s, std::int64_t delta) {
    switch (s) {
      case SlotState::FREE: counters_.free_n = static_cast<std::uint64_t>(static_cast<std::int64_t>(counters_.free_n) + delta); break;
      case SlotState::COPYING: counters_.copying = static_cast<std::uint64_t>(static_cast<std::int64_t>(counters_.copying) + delta); break;
      case SlotState::QUEUED: counters_.queued = static_cast<std::uint64_t>(static_cast<std::int64_t>(counters_.queued) + delta); break;
      case SlotState::INFERENCING: counters_.inferencing = static_cast<std::uint64_t>(static_cast<std::int64_t>(counters_.inferencing) + delta); break;
    }
  }

  void recount() {
    counters_.free_n = counters_.copying = counters_.queued = counters_.inferencing = 0;
    for (const auto s : states_) adjust(s, +1);
  }

  std::vector<SlotState> states_;
  SlotCounters counters_{};
};

}  // namespace face_async
