#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare nats.c opaque types using same style as nats.h (typedef struct),
// so they don't conflict when nats.h is included by the translation unit.
extern "C" {
  typedef struct __natsConnection   natsConnection;
  typedef struct __natsSubscription natsSubscription;
  typedef struct __natsMsg          natsMsg;
}

namespace aibox::adapter {

// =========================================================================
// NatsAdapter — thin RAII wrapper around the nats.c C client library.
//
// Features:
//   - Connect / Disconnect with built-in auto-reconnect (nats.c native)
//   - Wildcard Subscribe / Unsubscribe
//   - Publish (string + binary)
//   - Thread-safe state queries
//   - Pending-publish buffer when disconnected (drop-oldest, bounded)
//
// Design constraints (per plan v3):
//   - NO exit(1) on disconnect — publishers use "buffer + reject" rather
//     than killing the process
//   - Callbacks fire on NATS internal threads — callers MUST marshal to
//     their own thread/serial-queue before touching GStreamer objects
// =========================================================================
class NatsAdapter {
 public:
  // --- public types ---

  enum class State {
    kDisconnected,  // never connected, or explicitly disconnected
    kConnected,     // connected and operational
    kDegraded,      // was connected, now temporarily down (auto-reconnecting)
    kShuttingDown   // callbacks are being quiesced; connection is unavailable
  };

  struct Config {
    std::string nats_url     = "nats://localhost:4222";
    std::string client_name  = "aibox-pipeline";
    int         max_pending  = 1024;   // max buffered publishes when disconnected
  };

  // Message handler signature:
  //   subject — full NATS subject (including wildcard expansion)
  //   reply   — reply subject (often empty for pub/sub)
  //   payload — message body as a string
  //
  // ⚠️ Called from a NATS internal thread. Do NOT call GStreamer APIs
  //    directly from this callback.
  using MessageHandler = std::function<void(
      const std::string& subject,
      const std::string& reply,
      const std::string& payload)>;

  // --- lifecycle ---

  NatsAdapter();
  ~NatsAdapter();

  // Non-copyable, non-movable (natsConnection* is a stable internal pointer)
  NatsAdapter(const NatsAdapter&)            = delete;
  NatsAdapter& operator=(const NatsAdapter&) = delete;
  NatsAdapter(NatsAdapter&&)                 = delete;
  NatsAdapter& operator=(NatsAdapter&&)      = delete;

  // --- connection ---

  // Connect to the NATS server. Returns true on first successful connect.
  // On failure the adapter remains in kDisconnected state.
  bool Connect(const Config& cfg);

  // Graceful disconnect: drain pending publishes (best-effort), unsubscribe
  // all handlers, close the natsConnection. After this call the adapter is
  // back in kDisconnected and can be Connect()-ed again.
  void Disconnect();

  // Process-wide final NATS teardown. Call only after every NatsAdapter and
  // every other nats.c object has been destroyed; never from Disconnect().
  static bool CloseLibraryAndWait(int64_t timeout_ms = 5000);

  // --- state queries (thread-safe) ---

  State GetState() const;

  // IsAvailable — true when the adapter is connected (kConnected) and can
  // accept publishes.
  bool IsAvailable() const;

  // --- pub/sub ---

  // Subscribe to a subject with an optional wildcard.
  //   subject — e.g. "detection.frame"
  //   handler — called for every matching message (from NATS thread!)
  // Returns true on success.
  bool Subscribe(const std::string& subject, MessageHandler handler);

  // Remove a subscription. Safe to call with an unknown subject (no-op).
  bool Unsubscribe(const std::string& subject);

  // Publish a string payload. When disconnected the payload goes into a
  // bounded pending buffer (drop-oldest when full).
  bool Publish(const std::string& subject, const std::string& payload);

  // Publish binary payload.
  bool Publish(const std::string& subject, const std::vector<uint8_t>& payload);

  // --- diagnostics ---

  // Number of publishes currently buffered (waiting for reconnect).
  size_t PendingCount() const;

 private:
  // --- internal types ---

  struct PendingMessage {
    std::string              subject;
    std::vector<uint8_t>     payload;
    uint64_t                 enqueued_at_ms;  // monotonic timestamp
  };

  struct SubEntry {
    std::string         subject_pattern;
    MessageHandler      handler;
    natsSubscription*   sub;    // owned by natsConnection, non-owning ref
    std::atomic<bool>   accepting{true};
    std::atomic<unsigned int> callbacks_inflight{0};
    std::mutex          callbacks_mu;
    std::condition_variable callbacks_idle;
  };

  struct ConnectionCallbackContext {
    explicit ConnectionCallbackContext(uint64_t value) : generation(value) {}
    const uint64_t generation;
    std::atomic<NatsAdapter*> owner{nullptr};
    std::atomic<bool> accepting{false};
    std::atomic<bool> closed_notified{false};
    std::atomic<unsigned int> callbacks_inflight{0};
    std::mutex callbacks_mu;
    std::condition_variable callbacks_idle;
    std::mutex closed_mu;
    std::condition_variable closed_idle;
  };

  // --- NATS C callbacks (static, cast closure→this) ---

  static void OnNatsMessage(natsConnection* conn, natsSubscription* sub,
                            natsMsg* msg, void* closure);
  static void OnDisconnected(natsConnection* conn, void* closure);
  static void OnReconnected(natsConnection* conn, void* closure);
  static void OnClosed(natsConnection* conn, void* closure);

  // --- internal helpers ---

  // Drain pending publishes to the supplied live connection. Must hold mu_.
  void DrainPendingLocked(natsConnection* conn);
  void BufferPendingLocked(const std::string& subject,
                           const std::vector<uint8_t>& payload);
  void RetireSubscription(std::unique_ptr<SubEntry> entry);
  static bool WaitForConnectionClosed(ConnectionCallbackContext& context,
                                      int64_t timeout_ms);
  static void QuarantineConnectionContext(
      std::unique_ptr<ConnectionCallbackContext> context);
  static bool EnterCallback(std::atomic<bool>& accepting,
                            std::atomic<unsigned int>& inflight);
  static void LeaveCallback(std::atomic<unsigned int>& inflight,
                            std::mutex& wait_mu,
                            std::condition_variable& idle);
  static void WaitForCallbacks(std::atomic<unsigned int>& inflight,
                               std::mutex& wait_mu,
                               std::condition_variable& idle);

  // --- state ---

  Config                          config_;
  std::atomic<State>              state_{State::kDisconnected};
  natsConnection*                 conn_ = nullptr;

  // Serialises access to conn_ and guarantees that Disconnect does not
  // destroy the connection while another public operation is using it.
  mutable std::mutex              lifecycle_mu_;
  std::condition_variable         lifecycle_idle_;
  bool                            disconnecting_ = false;
  unsigned int                    active_operations_ = 0;
  uint64_t                        next_connection_generation_ = 1;
  std::unique_ptr<ConnectionCallbackContext> connection_callbacks_;

  // Pending publish buffer (bounded, drop-oldest)
  mutable std::mutex              mu_;
  std::vector<PendingMessage>     pending_;
  size_t                          pending_head_ = 0;   // ring buffer head

  // Active subscriptions (heap-allocated for stable closure pointers)
  std::vector<std::unique_ptr<SubEntry>> subs_;
};

}  // namespace aibox::adapter
