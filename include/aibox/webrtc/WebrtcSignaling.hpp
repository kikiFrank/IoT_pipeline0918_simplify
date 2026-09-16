#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/webrtc/WebrtcPayload.hpp"

namespace aibox::webrtc {

// =========================================================================
// WebrtcSignaling — NATS signaling layer for WebRTC sessions.
// Wraps NatsAdapter with typed callbacks for each message type.
// =========================================================================
class WebrtcSignaling {
 public:
  // Callback signatures
  using OnNewSession   = std::function<void(const SessionNewPayload&)>;
  using OnAnswer       = std::function<void(const SdpPayload&)>;
  using OnIceRemote    = std::function<void(const IceCandidatePayload&)>;
  using OnClose        = std::function<void(const SessionClosePayload&)>;
  using OnRenegotiate  = std::function<void(const RenegotiatePayload&)>;

  explicit WebrtcSignaling(adapter::NatsAdapter* nats);
  ~WebrtcSignaling();

  // Subscribe to all WebRTC wildcard subjects. Call after NatsAdapter is connected.
  bool Start(OnNewSession on_new, OnAnswer on_answer,
             OnIceRemote on_ice, OnClose on_close,
             OnRenegotiate on_reno);

  // Remove all subscriptions
  void Stop();

  // Publish helpers
  bool PublishOffer(const std::string& camera_id, const std::string& session_id,
                    const std::string& sdp);
  bool PublishIceLocal(const std::string& camera_id, const std::string& session_id,
                       const std::string& sdp_mid, int sdp_mline_index,
                       const std::string& candidate);
  bool PublishState(const std::string& camera_id, const std::string& session_id,
                    const std::string& ice_state, const std::string& peer_state);

  bool IsAvailable() const;

 private:
  adapter::NatsAdapter* nats_;
  mutable std::mutex mu_;
  bool started_ = false;
  bool stopping_ = false;
  std::shared_ptr<std::atomic<bool>> dispatch_accepting_;

  OnNewSession  on_new_;
  OnAnswer      on_answer_;
  OnIceRemote   on_ice_;
  OnClose       on_close_;
  OnRenegotiate on_reno_;

  // Subscription tracking
  std::string sub_new_;
  std::string sub_answer_;
  std::string sub_ice_;
  std::string sub_close_;
  std::string sub_reno_;

  // Internal handler for raw NATS messages
  void OnRawMessage(const std::string& subject, const std::string& reply,
                    const std::string& payload);
  void InvokeOnMain(std::function<void()> fn,
                    const std::shared_ptr<std::atomic<bool>>& accepting);
};

}  // namespace aibox::webrtc
