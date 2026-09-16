#include "aibox/adapter/NatsAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include <nats/nats.h>

namespace aibox::adapter {
namespace {

uint64_t NowMs() {
  return static_cast<uint64_t>(nats_Now());
}

const char* NatsStatusStr(natsStatus s) {
  switch (s) {
    case NATS_OK: return "OK";
    case NATS_ERR: return "ERR";
    case NATS_TIMEOUT: return "TIMEOUT";
    case NATS_CONNECTION_CLOSED: return "CONN_CLOSED";
    default: return "UNKNOWN";
  }
}

}  // namespace

NatsAdapter::NatsAdapter() = default;

NatsAdapter::~NatsAdapter() {
  Disconnect();
}

bool NatsAdapter::Connect(const Config& cfg) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
  if (conn_ || disconnecting_) {
    std::cerr << "[NatsAdapter] Already connected or disconnecting; reconnect rejected."
              << std::endl;
    return false;
  }

  config_ = cfg;
  state_.store(State::kDisconnected, std::memory_order_release);
  auto context = std::make_unique<ConnectionCallbackContext>(
      next_connection_generation_++);
  context->owner.store(this, std::memory_order_release);
  context->accepting.store(true, std::memory_order_release);

  natsOptions* opts = nullptr;
  natsStatus s = natsOptions_Create(&opts);
  if (s != NATS_OK) {
    std::cerr << "[NatsAdapter] natsOptions_Create failed: " << NatsStatusStr(s)
              << std::endl;
    return false;
  }

  natsOptions_SetURL(opts, cfg.nats_url.c_str());
  natsOptions_SetName(opts, cfg.client_name.c_str());
  natsOptions_SetAllowReconnect(opts, true);
  natsOptions_SetMaxReconnect(opts, -1);
  natsOptions_SetReconnectWait(opts, 100);
  natsOptions_SetReconnectBufSize(opts, 0);
  natsOptions_SetDisconnectedCB(opts, OnDisconnected, context.get());
  natsOptions_SetReconnectedCB(opts, OnReconnected, context.get());
  natsOptions_SetClosedCB(opts, OnClosed, context.get());

  s = natsConnection_Connect(&conn_, opts);
  natsOptions_Destroy(opts);
  if (s != NATS_OK) {
    conn_ = nullptr;
    std::cerr << "[NatsAdapter] Connect failed: " << NatsStatusStr(s)
              << " url=" << cfg.nats_url << std::endl;
    return false;
  }

  connection_callbacks_ = std::move(context);
  state_.store(State::kConnected, std::memory_order_release);
  std::cout << "[NatsAdapter] Connected to " << cfg.nats_url
            << " (client=" << cfg.client_name << ")" << std::endl;
  return true;
}

void NatsAdapter::Disconnect() {
  std::vector<std::unique_ptr<SubEntry>> subscriptions;
  std::unique_ptr<ConnectionCallbackContext> context;
  natsConnection* connection = nullptr;
  bool had_resources = false;

  {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mu_);
    if (disconnecting_) {
      lifecycle_idle_.wait(lifecycle_lock, [this] { return !disconnecting_; });
      return;
    }
    {
      std::lock_guard<std::mutex> data_lock(mu_);
      if (conn_ && state_.load(std::memory_order_acquire) == State::kConnected) {
        DrainPendingLocked(conn_);
      }
      had_resources = conn_ != nullptr || !subs_.empty() || !pending_.empty();
      subscriptions.swap(subs_);
      pending_.clear();
      pending_head_ = 0;
    }
    if (!had_resources) return;

    disconnecting_ = true;
    state_.store(State::kShuttingDown, std::memory_order_release);
    context = std::move(connection_callbacks_);
    if (context) {
      // Detach before destroying the connection: a late connection callback
      // may touch only its own context, never this adapter.
      context->owner.store(nullptr, std::memory_order_release);
      context->accepting.store(false, std::memory_order_release);
    }
    connection = conn_;
    conn_ = nullptr;
    lifecycle_idle_.wait(lifecycle_lock,
                         [this] { return active_operations_ == 0; });
  }

  for (auto& entry : subscriptions) RetireSubscription(std::move(entry));
  if (connection) natsConnection_Destroy(connection);
  if (context) {
    const bool closed = WaitForConnectionClosed(*context, 5000);
    WaitForCallbacks(context->callbacks_inflight, context->callbacks_mu,
                     context->callbacks_idle);
    if (!closed) {
      std::cerr << "[NatsAdapter] Connection close callback timed out; "
                << "quarantining generation " << context->generation << std::endl;
      QuarantineConnectionContext(std::move(context));
    }
  }

  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
    disconnecting_ = false;
    state_.store(State::kDisconnected, std::memory_order_release);
  }
  lifecycle_idle_.notify_all();
  std::cout << "[NatsAdapter] Disconnected." << std::endl;
}

bool NatsAdapter::CloseLibraryAndWait(int64_t timeout_ms) {
  const natsStatus status = nats_CloseAndWait(timeout_ms);
  if (status != NATS_OK) {
    std::cerr << "[NatsAdapter] nats_CloseAndWait failed: "
              << NatsStatusStr(status) << std::endl;
    return false;
  }
  return true;
}

NatsAdapter::State NatsAdapter::GetState() const {
  return state_.load(std::memory_order_acquire);
}

bool NatsAdapter::IsAvailable() const {
  return GetState() == State::kConnected;
}

bool NatsAdapter::Subscribe(const std::string& subject, MessageHandler handler) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
  if (!conn_ || disconnecting_) {
    std::cerr << "[NatsAdapter] Subscribe failed: not connected" << std::endl;
    return false;
  }
  std::lock_guard<std::mutex> data_lock(mu_);
  for (const auto& existing : subs_) {
    if (existing->subject_pattern == subject) {
      std::cerr << "[NatsAdapter] Duplicate subscription ignored: " << subject
                << std::endl;
      return false;
    }
  }

  auto entry = std::make_unique<SubEntry>();
  entry->subject_pattern = subject;
  entry->handler = std::move(handler);
  entry->sub = nullptr;
  SubEntry* raw_entry = entry.get();
  const natsStatus s = natsConnection_Subscribe(
      &raw_entry->sub, conn_, subject.c_str(), OnNatsMessage, raw_entry);
  if (s != NATS_OK) {
    std::cerr << "[NatsAdapter] Subscribe failed: " << NatsStatusStr(s)
              << " subject=" << subject << std::endl;
    return false;
  }
  subs_.push_back(std::move(entry));
  return true;
}

bool NatsAdapter::Unsubscribe(const std::string& subject) {
  std::unique_ptr<SubEntry> entry;
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
    std::lock_guard<std::mutex> data_lock(mu_);
    const auto it = std::find_if(subs_.begin(), subs_.end(),
                                 [&subject](const auto& candidate) {
                                   return candidate->subject_pattern == subject;
                                 });
    if (it == subs_.end()) return false;
    entry = std::move(*it);
    subs_.erase(it);
    ++active_operations_;
  }

  RetireSubscription(std::move(entry));

  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
    --active_operations_;
  }
  lifecycle_idle_.notify_all();
  return true;
}

bool NatsAdapter::Publish(const std::string& subject, const std::string& payload) {
  return Publish(subject, std::vector<uint8_t>(payload.begin(), payload.end()));
}

bool NatsAdapter::Publish(const std::string& subject,
                          const std::vector<uint8_t>& payload) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
  const State state = state_.load(std::memory_order_acquire);
  if (!conn_ || disconnecting_ || state == State::kShuttingDown) {
    std::cerr << "[NatsAdapter] Publish failed: not connected" << std::endl;
    return false;
  }
  if (state != State::kConnected) {
    std::lock_guard<std::mutex> data_lock(mu_);
    BufferPendingLocked(subject, payload);
    return true;
  }
  const natsStatus s = natsConnection_Publish(
      conn_, subject.c_str(), payload.data(), static_cast<int>(payload.size()));
  if (s != NATS_OK) {
    std::cerr << "[NatsAdapter] Publish failed: " << NatsStatusStr(s)
              << " subject=" << subject << std::endl;
    return false;
  }
  return true;
}

size_t NatsAdapter::PendingCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_.size();
}

void NatsAdapter::OnNatsMessage(natsConnection*, natsSubscription*,
                                natsMsg* msg, void* closure) {
  auto* entry = static_cast<SubEntry*>(closure);
  if (!entry || !EnterCallback(entry->accepting, entry->callbacks_inflight)) {
    if (msg) natsMsg_Destroy(msg);
    return;
  }

  const char* subject = natsMsg_GetSubject(msg);
  const char* reply = natsMsg_GetReply(msg);
  const char* data = natsMsg_GetData(msg);
  const int length = natsMsg_GetDataLength(msg);
  const std::string subject_copy(subject ? subject : "");
  const std::string reply_copy(reply ? reply : "");
  const std::string payload_copy(
      data ? std::string(data, static_cast<size_t>(length)) : "");
  natsMsg_Destroy(msg);

  if (entry->handler) entry->handler(subject_copy, reply_copy, payload_copy);
  LeaveCallback(entry->callbacks_inflight, entry->callbacks_mu,
                entry->callbacks_idle);
}

void NatsAdapter::OnDisconnected(natsConnection* conn, void* closure) {
  auto* context = static_cast<ConnectionCallbackContext*>(closure);
  if (!context || !EnterCallback(context->accepting, context->callbacks_inflight)) return;
  NatsAdapter* self = context->owner.load(std::memory_order_acquire);
  if (self) {
    std::lock_guard<std::mutex> lifecycle_lock(self->lifecycle_mu_);
    if (!self->disconnecting_ && self->conn_ == conn) {
      self->state_.store(State::kDegraded, std::memory_order_release);
      std::cerr << "[NatsAdapter] Disconnected from NATS - buffering publishes"
                << std::endl;
    }
  }
  LeaveCallback(context->callbacks_inflight, context->callbacks_mu,
                context->callbacks_idle);
}

void NatsAdapter::OnReconnected(natsConnection* conn, void* closure) {
  auto* context = static_cast<ConnectionCallbackContext*>(closure);
  if (!context || !EnterCallback(context->accepting, context->callbacks_inflight)) return;
  NatsAdapter* self = context->owner.load(std::memory_order_acquire);
  if (self) {
    std::lock_guard<std::mutex> lifecycle_lock(self->lifecycle_mu_);
    if (!self->disconnecting_ && self->conn_ == conn) {
      self->state_.store(State::kConnected, std::memory_order_release);
      std::lock_guard<std::mutex> data_lock(self->mu_);
      self->DrainPendingLocked(conn);
      std::cout << "[NatsAdapter] Reconnected to NATS" << std::endl;
    }
  }
  LeaveCallback(context->callbacks_inflight, context->callbacks_mu,
                context->callbacks_idle);
}

void NatsAdapter::OnClosed(natsConnection* conn, void* closure) {
  auto* context = static_cast<ConnectionCallbackContext*>(closure);
  if (!context) return;
  context->callbacks_inflight.fetch_add(1, std::memory_order_acq_rel);
  context->closed_notified.store(true, std::memory_order_release);
  context->closed_idle.notify_all();

  // Disconnect detaches owner before natsConnection_Destroy(). A delayed
  // closed callback is still expected, but must not enter a dead adapter.
  NatsAdapter* self = context->owner.load(std::memory_order_acquire);
  if (self && context->accepting.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lifecycle_lock(self->lifecycle_mu_);
    if (!self->disconnecting_ && self->conn_ == conn) {
      self->state_.store(State::kDisconnected, std::memory_order_release);
      std::cerr << "[NatsAdapter] Connection closed (permanent)" << std::endl;
    }
  }
  LeaveCallback(context->callbacks_inflight, context->callbacks_mu,
                context->callbacks_idle);
}

void NatsAdapter::BufferPendingLocked(const std::string& subject,
                                      const std::vector<uint8_t>& payload) {
  if (config_.max_pending <= 0) return;
  if (pending_.size() >= static_cast<size_t>(config_.max_pending)) {
    pending_[pending_head_] = {subject, payload, NowMs()};
    pending_head_ = (pending_head_ + 1) % pending_.size();
    return;
  }
  pending_.push_back({subject, payload, NowMs()});
}

void NatsAdapter::DrainPendingLocked(natsConnection* conn) {
  if (!conn || pending_.empty()) return;
  size_t drained = 0;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const size_t index = (pending_head_ + i) % pending_.size();
    const auto& message = pending_[index];
    if (natsConnection_Publish(conn, message.subject.c_str(), message.payload.data(),
                               static_cast<int>(message.payload.size())) == NATS_OK) {
      ++drained;
    }
  }
  pending_.clear();
  pending_head_ = 0;
  if (drained > 0) {
    std::cout << "[NatsAdapter] Drained " << drained << " pending publishes"
              << std::endl;
  }
}

void NatsAdapter::RetireSubscription(std::unique_ptr<SubEntry> entry) {
  if (!entry) return;
  entry->accepting.store(false, std::memory_order_release);
  if (entry->sub) {
    natsSubscription_Unsubscribe(entry->sub);
    natsSubscription_Destroy(entry->sub);
    entry->sub = nullptr;
  }
  WaitForCallbacks(entry->callbacks_inflight, entry->callbacks_mu,
                   entry->callbacks_idle);
}

bool NatsAdapter::WaitForConnectionClosed(ConnectionCallbackContext& context,
                                          int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(context.closed_mu);
  return context.closed_idle.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), [&context] {
        return context.closed_notified.load(std::memory_order_acquire);
      });
}

void NatsAdapter::QuarantineConnectionContext(
    std::unique_ptr<ConnectionCallbackContext> context) {
  // A nats.c callback owns only a raw closure pointer. If its close callback
  // did not arrive in time, intentionally retain the detached context for the
  // rest of the process instead of turning a shutdown delay into a UAF.
  static auto* quarantine =
      new std::vector<std::unique_ptr<ConnectionCallbackContext>>();
  static auto* quarantine_mu = new std::mutex();
  std::lock_guard<std::mutex> lock(*quarantine_mu);
  quarantine->push_back(std::move(context));
}

bool NatsAdapter::EnterCallback(std::atomic<bool>& accepting,
                                std::atomic<unsigned int>& inflight) {
  if (!accepting.load(std::memory_order_acquire)) return false;
  inflight.fetch_add(1, std::memory_order_acq_rel);
  if (accepting.load(std::memory_order_acquire)) return true;
  inflight.fetch_sub(1, std::memory_order_acq_rel);
  return false;
}

void NatsAdapter::LeaveCallback(std::atomic<unsigned int>& inflight,
                                std::mutex& wait_mu,
                                std::condition_variable& idle) {
  if (inflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lock(wait_mu);
    idle.notify_all();
  }
}

void NatsAdapter::WaitForCallbacks(std::atomic<unsigned int>& inflight,
                                   std::mutex& wait_mu,
                                   std::condition_variable& idle) {
  std::unique_lock<std::mutex> lock(wait_mu);
  idle.wait(lock, [&inflight] {
    return inflight.load(std::memory_order_acquire) == 0;
  });
}

}  // namespace aibox::adapter
