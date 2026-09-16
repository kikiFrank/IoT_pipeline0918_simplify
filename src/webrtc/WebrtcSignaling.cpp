#include "aibox/webrtc/WebrtcSignaling.hpp"

#include <iostream>
#include <memory>
#include <vector>

#include <glib.h>

namespace aibox::webrtc {

namespace {
struct MainContextCall {
  std::function<void()> fn;
  std::shared_ptr<std::atomic<bool>> accepting;
};

gboolean RunMainContextCall(gpointer user_data) {
  std::unique_ptr<MainContextCall> call(static_cast<MainContextCall*>(user_data));
  if (call->accepting && call->accepting->load(std::memory_order_acquire))
    call->fn();
  return G_SOURCE_REMOVE;
}
}  // namespace

// Per-session wildcard subjects (suffixes for session.* subscriptions)
static const char* kSubNew    = "webrtc.cam.*.session.new";
static const char* kSubAnswer = "webrtc.cam.*.session.*.answer";
static const char* kSubIce    = "webrtc.cam.*.session.*.ice.remote";
static const char* kSubClose  = "webrtc.cam.*.session.*.close";
static const char* kSubReno   = "webrtc.cam.*.session.*.renegotiate";

WebrtcSignaling::WebrtcSignaling(adapter::NatsAdapter* nats) : nats_(nats) {}

WebrtcSignaling::~WebrtcSignaling() { Stop(); }

bool WebrtcSignaling::Start(OnNewSession on_new, OnAnswer on_answer,
                            OnIceRemote on_ice, OnClose on_close,
                            OnRenegotiate on_reno) {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_ || stopping_) {
    std::cerr << "[WebrtcSignaling] Start rejected: lifecycle transition active"
              << std::endl;
    return false;
  }
  if (!nats_ || !nats_->IsAvailable()) {
    std::cerr << "[WebrtcSignaling] NATS not available" << std::endl;
    return false;
  }

  on_new_    = std::move(on_new);
  on_answer_ = std::move(on_answer);
  on_ice_    = std::move(on_ice);
  on_close_  = std::move(on_close);
  on_reno_   = std::move(on_reno);
  dispatch_accepting_ = std::make_shared<std::atomic<bool>>(true);
  started_ = true;

  auto handler = [this](const std::string& subject, const std::string& reply,
                        const std::string& payload) {
    OnRawMessage(subject, reply, payload);
  };

  if (!nats_->Subscribe(kSubNew, handler)) {
    started_ = false;
    std::cerr << "[WebrtcSignaling] Failed to subscribe: " << kSubNew << std::endl;
    return false;
  }
  sub_new_ = kSubNew;

  if (!nats_->Subscribe(kSubAnswer, handler)) {
    started_ = false;
    std::cerr << "[WebrtcSignaling] Failed to subscribe: " << kSubAnswer << std::endl;
    return false;
  }
  sub_answer_ = kSubAnswer;

  if (!nats_->Subscribe(kSubIce, handler)) {
    started_ = false;
    std::cerr << "[WebrtcSignaling] Failed to subscribe: " << kSubIce << std::endl;
    return false;
  }
  sub_ice_ = kSubIce;

  if (!nats_->Subscribe(kSubClose, handler)) {
    started_ = false;
    std::cerr << "[WebrtcSignaling] Failed to subscribe: " << kSubClose << std::endl;
    return false;
  }
  sub_close_ = kSubClose;

  if (!nats_->Subscribe(kSubReno, handler)) {
    started_ = false;
    std::cerr << "[WebrtcSignaling] Failed to subscribe: " << kSubReno << std::endl;
    return false;
  }
  sub_reno_ = kSubReno;

  std::cout << "[WebrtcSignaling] Subscribed to 5 wildcard subjects" << std::endl;
  return true;
}

void WebrtcSignaling::Stop() {
  std::vector<std::string> subscriptions;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || (!started_ && sub_new_.empty() && sub_answer_.empty() &&
                      sub_ice_.empty() && sub_close_.empty() && sub_reno_.empty())) {
      return;
    }
    stopping_ = true;
    if (dispatch_accepting_)
      dispatch_accepting_->store(false, std::memory_order_release);
    if (!sub_new_.empty()) subscriptions.push_back(std::move(sub_new_));
    if (!sub_answer_.empty()) subscriptions.push_back(std::move(sub_answer_));
    if (!sub_ice_.empty()) subscriptions.push_back(std::move(sub_ice_));
    if (!sub_close_.empty()) subscriptions.push_back(std::move(sub_close_));
    if (!sub_reno_.empty()) subscriptions.push_back(std::move(sub_reno_));
  }
  for (const auto& subject : subscriptions) {
    if (nats_) nats_->Unsubscribe(subject);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    on_new_ = nullptr;
    on_answer_ = nullptr;
    on_ice_ = nullptr;
    on_close_ = nullptr;
    on_reno_ = nullptr;
    dispatch_accepting_.reset();
    started_ = false;
    stopping_ = false;
  }
  std::cout << "[WebrtcSignaling] Stopped" << std::endl;
}

void WebrtcSignaling::InvokeOnMain(
    std::function<void()> fn, const std::shared_ptr<std::atomic<bool>>& accepting) {
  auto* call = new MainContextCall{std::move(fn), accepting};
  g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT,
                             &RunMainContextCall, call, nullptr);
}

bool WebrtcSignaling::IsAvailable() const {
  return nats_ && nats_->IsAvailable();
}

// =========================================================================
// Publish helpers
// =========================================================================

bool WebrtcSignaling::PublishOffer(const std::string& camera_id,
                                   const std::string& session_id,
                                   const std::string& sdp) {
  if (!nats_) return false;
  std::string subject = MakeSessionSubject(camera_id, session_id, "offer");
  std::string json = BuildSdpJson(camera_id, session_id, sdp, "offer");
  return nats_->Publish(subject, json);
}

bool WebrtcSignaling::PublishIceLocal(const std::string& camera_id,
                                      const std::string& session_id,
                                      const std::string& sdp_mid,
                                      int sdp_mline_index,
                                      const std::string& candidate) {
  if (!nats_) return false;
  std::string subject = MakeSessionSubject(camera_id, session_id, "ice.local");
  std::string json = BuildIceJson(camera_id, session_id, sdp_mid,
                                  sdp_mline_index, candidate);
  return nats_->Publish(subject, json);
}

bool WebrtcSignaling::PublishState(const std::string& camera_id,
                                   const std::string& session_id,
                                   const std::string& ice_state,
                                   const std::string& peer_state) {
  if (!nats_) return false;
  std::string subject = MakeSessionSubject(camera_id, session_id, "state");
  std::string json = BuildStateJson(camera_id, session_id, ice_state, peer_state);
  bool ok = nats_->Publish(subject, json);
  std::cerr << "[WebrtcSignaling] TX: " << subject
            << " ice=" << ice_state << " peer=" << peer_state
            << " ok=" << (ok ? "1" : "0") << std::endl;
  return ok;
}

// =========================================================================
// Message dispatch
// =========================================================================

void WebrtcSignaling::OnRawMessage(const std::string& subject,
                                   const std::string& /*reply*/,
                                   const std::string& payload) {
  OnNewSession on_new;
  OnAnswer on_answer;
  OnIceRemote on_ice;
  OnClose on_close;
  OnRenegotiate on_reno;
  std::shared_ptr<std::atomic<bool>> accepting;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ || stopping_ || !dispatch_accepting_) return;
    on_new = on_new_;
    on_answer = on_answer_;
    on_ice = on_ice_;
    on_close = on_close_;
    on_reno = on_reno_;
    accepting = dispatch_accepting_;
  }
  std::cerr << "[WebrtcSignaling] RX: " << subject << " payload="
            << payload.size() << "b json=" << payload.substr(0,80) << std::endl;
  std::string camera_id, session_id, suffix;
  if (!ParseSessionSubject(subject, camera_id, session_id, suffix)) {
    if (subject.compare(0, 11, "webrtc.cam.") == 0) {
      auto dot = subject.find(".session.", 11);
      if (dot != std::string::npos) {
        camera_id = subject.substr(11, dot - 11);
        suffix = subject.substr(dot + 9);
        if (suffix.find('.') == std::string::npos) {
          if (suffix == "new" && on_new) {
            auto p = ParseSessionNew(payload);
            p.camera_id = camera_id;
            InvokeOnMain([on_new = std::move(on_new), p = std::move(p)]() {
              on_new(p);
            }, accepting);
          }
          return;
        }
      }
    }
    std::cerr << "[WebrtcSignaling] Unparseable subject: " << subject << std::endl;
    return;
  }

  if (suffix == "answer" && on_answer) {
    auto p = ParseSdpPayload(payload);
    p.camera_id = camera_id;
    p.session_id = session_id;
    InvokeOnMain([on_answer = std::move(on_answer), p = std::move(p)]() {
      on_answer(p);
    }, accepting);
  } else if (suffix == "ice.remote" && on_ice) {
    auto p = ParseIceCandidate(payload);
    p.camera_id = camera_id;
    p.session_id = session_id;
    InvokeOnMain([on_ice = std::move(on_ice), p = std::move(p)]() {
      on_ice(p);
    }, accepting);
  } else if (suffix == "close" && on_close) {
    auto p = ParseSessionClose(payload);
    p.camera_id = camera_id;
    p.session_id = session_id;
    InvokeOnMain([on_close = std::move(on_close), p = std::move(p)]() {
      on_close(p);
    }, accepting);
  } else if (suffix == "renegotiate" && on_reno) {
    auto p = ParseRenegotiate(payload);
    p.camera_id = camera_id;
    p.session_id = session_id;
    InvokeOnMain([on_reno = std::move(on_reno), p = std::move(p)]() {
      on_reno(p);
    }, accepting);
  } else {
    std::cerr << "[WebrtcSignaling] Unknown suffix: " << suffix
              << " subject=" << subject << std::endl;
  }
}

}  // namespace aibox::webrtc
