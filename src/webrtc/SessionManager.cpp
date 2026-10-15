#include "aibox/webrtc/SessionManager.hpp"
#include "aibox/webrtc/WrtcTrace.hpp"
#include "aibox/pipeline/GstElementHelper.hpp"
#include "aibox/pipeline/GstPipelineBuilder.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

namespace aibox::webrtc {

SessionManager::SessionManager() = default;

SessionManager::~SessionManager() { Shutdown(); }

bool SessionManager::Init(GstElement* pipeline, adapter::NatsAdapter* nats,
                          pipeline::RuntimeRegistry* registry) {
  std::lock_guard<std::mutex> lock(mu_);
  pipeline_    = pipeline;
  nats_        = nats;
  registry_    = registry;
  initialized_ = true;
  shutting_down_ = false;
  std::cout << "[SessionManager] Initialized" << std::endl;
  return true;
}

void SessionManager::RegisterCamera(const std::string& camera_id,
                                    GstElement* tee_webrtc) {
  std::lock_guard<std::mutex> lock(mu_);
  webrtc_tees_[camera_id] = tee_webrtc;  // may be nullptr until EnsureWebrtcEncoder
  encoder_state_by_camera_[camera_id] = tee_webrtc
      ? EncoderState::kReady : EncoderState::kAbsent;
  std::cout << "[SessionManager] Registered camera: " << camera_id
            << (tee_webrtc ? " (tee ready)" : " (lazy stub)") << std::endl;
}

int SessionManager::EncoderGeneration(const std::string& camera_id) const {
  std::lock_guard<std::mutex> lock(generation_mu_);
  auto it = encoder_generation_by_camera_.find(camera_id);
  return it == encoder_generation_by_camera_.end() ? 0 : it->second;
}

bool SessionManager::HasLiveSessionLocked(const std::string& camera_id) const {
  return std::any_of(sessions_.begin(), sessions_.end(),
      [&camera_id](const auto& item) {
        return item.second && item.second->GetCameraId() == camera_id &&
            item.second->GetState() != WebRtcSession::State::kClosed;
      });
}

bool SessionManager::CameraHasPrivateBranchLocked(
    const std::string& camera_id) const {
  if (HasLiveSessionLocked(camera_id)) return true;
  return std::any_of(closing_sessions_.begin(), closing_sessions_.end(),
      [&camera_id](const auto& item) {
        return item.second && item.second->GetCameraId() == camera_id;
      });
}

void SessionManager::ScheduleEncoderBuildLocked() {
  // One structure at a time, and the next camera waits for this camera's
  // first encoded frame (Venus). Offer/Init already ran at structure-up.
  if (encoder_build_source_ || encoder_build_queue_.empty() || shutting_down_)
    return;
  if (encoder_build_in_progress_) return;
  encoder_build_source_ =
      g_timeout_add(1, &SessionManager::EncoderBuildCb, this);
}

void SessionManager::QueueSessionInitLocked(const std::string& camera_id,
                                            const std::string& session_id) {
  if (shutting_down_) {
    std::cerr << "[SessionManager] QueueSessionInitLocked refused cam="
              << camera_id << " (shutting down)\n";
    return;
  }
  auto tee_it = webrtc_tees_.find(camera_id);
  const bool tee_ready = tee_it != webrtc_tees_.end() && tee_it->second;
  auto state_it = encoder_state_by_camera_.find(camera_id);
  const EncoderState state = state_it == encoder_state_by_camera_.end()
      ? EncoderState::kAbsent : state_it->second;
  if (tee_ready) {
    auto session = sessions_.find(session_id);
    if (session != sessions_.end()) session->second->Init(pipeline_, tee_it->second);
    return;
  }

  auto& pending = pending_init_by_camera_[camera_id];
  pending.push_back(session_id);
  if (state == EncoderState::kAbsent || state == EncoderState::kFailed) {
    encoder_state_by_camera_[camera_id] = EncoderState::kQueued;
    encoder_build_queue_.push_back(camera_id);
    std::cerr << "[CP05-R1B] encoder state " << camera_id
              << " ABSENT→QUEUED pending=" << pending.size() << std::endl;
  } else {
    std::cerr << "[CP05-R1B] encoder wait " << camera_id
              << " state=" << static_cast<int>(state)
              << " pending=" << pending.size() << std::endl;
  }
  ScheduleEncoderBuildLocked();
}

gboolean SessionManager::EncoderBuildCb(gpointer user_data) {
  auto* self = static_cast<SessionManager*>(user_data);
  self->RunEncoderBuild();
  return G_SOURCE_REMOVE;
}

void SessionManager::CancelFirstFrameTimeoutLocked(const std::string& camera_id) {
  auto it = first_frame_timeout_by_camera_.find(camera_id);
  if (it == first_frame_timeout_by_camera_.end()) return;
  if (it->second) g_source_remove(it->second);
  first_frame_timeout_by_camera_.erase(it);
}

void SessionManager::ArmFirstFrameTimeoutLocked(const std::string& camera_id,
                                                int generation, guint seconds) {
  CancelFirstFrameTimeoutLocked(camera_id);
  auto* tctx = new FirstFrameTimeout{this, camera_id, generation};
  first_frame_timeout_by_camera_[camera_id] =
      g_timeout_add_seconds(seconds, &SessionManager::FirstFrameTimeoutCb, tctx);
}

void SessionManager::RunEncoderBuild() {
  std::string camera_id;
  int generation = 0;
  GstElement* tee = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;  // no new builds once Shutdown started
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    encoder_build_source_ = 0;
    if (encoder_build_in_progress_ || shutting_down_) return;

    while (!encoder_build_queue_.empty()) {
      camera_id = std::move(encoder_build_queue_.front());
      encoder_build_queue_.pop_front();
      auto state_it = encoder_state_by_camera_.find(camera_id);
      if (state_it != encoder_state_by_camera_.end() &&
          state_it->second == EncoderState::kQueued) {
        break;
      }
      camera_id.clear();
    }
    if (camera_id.empty()) return;

    encoder_state_by_camera_[camera_id] = EncoderState::kBuilding;
    encoder_build_in_progress_ = true;
    encoder_build_camera_ = camera_id;
    std::cerr << "[CP05-R1B] encoder state " << camera_id
              << " QUEUED→BUILDING (await first frame)" << std::endl;
  }

  // Build without holding mu_ (pad/state waits).
  tee = BuildEncoderChain(camera_id);
  generation = EncoderGeneration(camera_id);

  if (!tee) {
    FailEncoderBuild(camera_id, "BuildEncoderChain returned null");
    return;
  }

  std::vector<std::string> pending_ids;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_ || encoder_build_camera_ != camera_id) {
      return;
    }
    encoder_build_generation_ = generation;
    ArmFirstFrameTimeoutLocked(camera_id, generation, 25);
    pending_ids = std::move(pending_init_by_camera_[camera_id]);
    pending_init_by_camera_.erase(camera_id);
    // Keep encoder_build_in_progress_ until first-frame so the next Venus
    // chain does not start yet (cam-006 raw=0 when 6 structures overlap).
    std::cerr << "[CP05-R1B] encoder " << camera_id
              << " gen=" << generation
              << " structure up; Init now (offer not gated on first frame)"
              << " pending_init=" << pending_ids.size() << std::endl;
  }
  if (!pending_ids.empty()) {
    auto* d = new DeferredInitCtx{this, std::move(pending_ids), camera_id};
    g_idle_add(&SessionManager::DeferredSessionInitCb, d);
  }
}

gboolean SessionManager::FirstFrameIdleCb(gpointer user_data) {
  auto* ctx = static_cast<FirstFrameNotify*>(user_data);
  ctx->self->CompleteEncoderReady(ctx->camera_id, ctx->generation);
  delete ctx;
  return G_SOURCE_REMOVE;
}

gboolean SessionManager::FirstFrameTimeoutCb(gpointer user_data) {
  auto* ctx = static_cast<FirstFrameTimeout*>(user_data);
  SessionManager* self = ctx->self;
  const std::string camera_id = ctx->camera_id;
  const int generation = ctx->generation;
  delete ctx;

  bool still_waiting = false;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    self->first_frame_timeout_by_camera_.erase(camera_id);
    auto state_it = self->encoder_state_by_camera_.find(camera_id);
    still_waiting =
        state_it != self->encoder_state_by_camera_.end() &&
        state_it->second == EncoderState::kBuilding &&
        self->EncoderGeneration(camera_id) == generation;
  }
  if (!still_waiting) return G_SOURCE_REMOVE;

  std::cerr << "[CP05-R1B] encoder first-frame TIMEOUT cam=" << camera_id
            << " gen=" << generation << std::endl;
  // 1) soft-extend once (keep chain, no Venus teardown)
  // 2) teardown+requeue up to 2 times
  // 3) FAILED
  if (self->SoftExtendFirstFrameOnce(camera_id, generation)) {
    return G_SOURCE_REMOVE;
  }
  if (!self->RetryEncoderBuildOnce(camera_id, "first-frame timeout")) {
    self->FailEncoderBuild(camera_id, "first-frame timeout");
  }
  return G_SOURCE_REMOVE;
}

gboolean SessionManager::DeferredSessionInitCb(gpointer user_data) {
  auto* ctx = static_cast<DeferredInitCtx*>(user_data);
  GstElement* tee = nullptr;
  std::vector<WebRtcSession*> sessions;
  bool build_busy = false;
  {
    std::lock_guard<std::mutex> lock(ctx->self->mu_);
    // Per-cam offer: this camera is already READY. Do not wait for another
    // camera's first-frame (that gated 001-005 offers behind cam-006).
    // DoInit itself is rate-limited to 1/tick in ProcessAllQueues.
    build_busy = ctx->self->encoder_build_in_progress_;
    auto tee_it = ctx->self->webrtc_tees_.find(ctx->camera_id);
    tee = (tee_it == ctx->self->webrtc_tees_.end()) ? nullptr : tee_it->second;
    for (const auto& sid : ctx->session_ids) {
      auto it = ctx->self->sessions_.find(sid);
      if (it != ctx->self->sessions_.end()) sessions.push_back(it->second.get());
    }
  }
  if (build_busy) {
    std::cerr << "[CP05-R1B] DeferredSessionInit cam=" << ctx->camera_id
              << " sessions=" << sessions.size()
              << " proceeding while another encoder is BUILDING"
              << std::endl;
  }
  for (auto* session : sessions) {
    if (tee) session->Init(ctx->self->pipeline_, tee);
  }
  delete ctx;
  return G_SOURCE_REMOVE;
}

void SessionManager::CompleteEncoderReady(const std::string& camera_id,
                                          int generation) {
  std::vector<std::string> pending_ids;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;
    auto state_it = encoder_state_by_camera_.find(camera_id);
    if (state_it == encoder_state_by_camera_.end() ||
        state_it->second != EncoderState::kBuilding ||
        EncoderGeneration(camera_id) != generation) {
      return;
    }
    CancelFirstFrameTimeoutLocked(camera_id);
    encoder_state_by_camera_[camera_id] = EncoderState::kReady;
    first_frame_retry_by_camera_.erase(camera_id);
    first_frame_soft_extend_by_camera_.erase(camera_id);
    if (encoder_build_camera_ == camera_id) {
      encoder_build_in_progress_ = false;
      encoder_build_camera_.clear();
      encoder_build_generation_ = 0;
    }

    pending_ids = std::move(pending_init_by_camera_[camera_id]);
    pending_init_by_camera_.erase(camera_id);
    std::cerr << "[CP05-R1B] encoder state " << camera_id
              << " BUILDING→READY gen=" << generation
              << " sessions=" << pending_ids.size()
              << " (first-frame; offer already decoupled)" << std::endl;
    ScheduleEncoderBuildLocked();
  }
  if (!pending_ids.empty()) {
    auto* d = new DeferredInitCtx{this, std::move(pending_ids), camera_id};
    g_idle_add(&SessionManager::DeferredSessionInitCb, d);
  }
}

bool SessionManager::SoftExtendFirstFrameOnce(const std::string& camera_id,
                                              int generation) {
  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_) return false;
  auto state_it = encoder_state_by_camera_.find(camera_id);
  if (state_it == encoder_state_by_camera_.end() ||
      state_it->second != EncoderState::kBuilding ||
      EncoderGeneration(camera_id) != generation) {
    return false;
  }
  int& extends = first_frame_soft_extend_by_camera_[camera_id];
  if (extends >= 1) return false;
  ++extends;
  ArmFirstFrameTimeoutLocked(camera_id, generation, 20);
  std::cerr << "[CP05-R1B] encoder first-frame EXTEND cam=" << camera_id
            << " gen=" << generation << " +20s (no teardown)" << std::endl;
  return true;
}

bool SessionManager::RetryEncoderBuildOnce(const std::string& camera_id,
                                           const char* reason) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    int& retries = first_frame_retry_by_camera_[camera_id];
    if (retries >= 2) return false;
    ++retries;
    CancelFirstFrameTimeoutLocked(camera_id);
    // Soft-extend budget resets for the rebuilt generation.
    first_frame_soft_extend_by_camera_.erase(camera_id);
    if (encoder_build_camera_ == camera_id) {
      encoder_build_in_progress_ = false;
      encoder_build_camera_.clear();
      encoder_build_generation_ = 0;
    }
    // Keep pending sessions; requeue this camera ahead of later builds.
    encoder_state_by_camera_[camera_id] = EncoderState::kQueued;
    encoder_build_queue_.push_front(camera_id);
    std::cerr << "[CP05-R1B] encoder state " << camera_id
              << " BUILDING→REQUEUE reason=" << (reason ? reason : "?")
              << " retry=" << retries
              << " pending=" << pending_init_by_camera_[camera_id].size()
              << std::endl;
  }
  TeardownEncoderChain(camera_id);
  {
    std::lock_guard<std::mutex> lock(mu_);
    ScheduleEncoderBuildLocked();
  }
  return true;
}

void SessionManager::FailEncoderBuild(const std::string& camera_id,
                                      const char* reason) {
  std::vector<std::string> failed_sessions;
  {
    std::lock_guard<std::mutex> lock(mu_);
    CancelFirstFrameTimeoutLocked(camera_id);
    if (encoder_build_camera_ == camera_id) {
      encoder_build_in_progress_ = false;
      encoder_build_camera_.clear();
      encoder_build_generation_ = 0;
    }
    first_frame_retry_by_camera_.erase(camera_id);
    first_frame_soft_extend_by_camera_.erase(camera_id);
    encoder_state_by_camera_[camera_id] = EncoderState::kFailed;
    failed_sessions = std::move(pending_init_by_camera_[camera_id]);
    pending_init_by_camera_.erase(camera_id);
    /* NOTE: do NOT erase enc_probe_ctx_by_camera_ here — the EncOutProbe is
     * still mounted with its raw holder; TeardownEncoderChain removes the
     * probe FIRST and destroys the ctx LAST (UAF otherwise). */
    std::cerr << "[CP05-R1B] encoder state " << camera_id
              << " BUILDING→FAILED reason=" << (reason ? reason : "?")
              << " sessions=" << failed_sessions.size() << std::endl;
  }
  // Cleanup half-built chain outside mu_ (may touch pads/state).
  std::cout << "[CP05-TD] FailEncoderBuild teardown begin cam=" << camera_id
            << std::endl;
  TeardownEncoderChain(camera_id);
  std::cout << "[CP05-TD] FailEncoderBuild teardown done cam=" << camera_id
            << std::endl;
  for (const auto& sid : failed_sessions) RemoveSession(sid);
  {
    std::lock_guard<std::mutex> lock(mu_);
    ScheduleEncoderBuildLocked();
  }
}

GstElement* SessionManager::EnsureWebrtcEncoder(const std::string& camera_id) {
  // NOTE: Do NOT lock mu_ here. Called from WebRtcSession::DoInit via
  // ProcessAllQueues() which already holds mu_ (would deadlock).
  // Only safe on the main-loop drain path.

  auto tee_it = webrtc_tees_.find(camera_id);
  if (tee_it != webrtc_tees_.end() && tee_it->second)
    return tee_it->second;

  if (!registry_ || !pipeline_) {
    std::cerr << "[SessionManager] EnsureWebrtcEncoder: no registry/pipeline\n";
    return nullptr;
  }

  // If registry already has a tee (race), use it.
  auto reg_tee = registry_->webrtc_tee_by_camera.find(camera_id);
  if (reg_tee != registry_->webrtc_tee_by_camera.end() && reg_tee->second) {
    webrtc_tees_[camera_id] = reg_tee->second;
    return reg_tee->second;
  }

  // CP05-R1B: never sync-build here. Creation is serial + first-frame gated
  // via QueueSessionInitLocked / RunEncoderBuild / CompleteEncoderReady.
  std::cerr << "[CP05-R1B] EnsureWebrtcEncoder: no tee yet for " << camera_id
            << " (queue/first-frame gated; not sync-building)\n";
  return nullptr;
}

void SessionManager::EncProbeHolderFree(gpointer data) {
  /* destroy notify: runs after gst_pad_remove_probe has finished with all
   * in-flight callbacks — releasing the shared_ptr HERE is the only safe
   * place to drop the last reference. */
  delete static_cast<EncProbeHolder*>(data);
}

void SessionManager::HealthProbeHolderFree(gpointer data) {
  delete static_cast<HealthProbeHolder*>(data);
}

GstPadProbeReturn SessionManager::EncOutProbe(GstPad* /*pad*/,
                                              GstPadProbeInfo* /*info*/,
                                              gpointer user_data) {
  auto* holder = static_cast<EncProbeHolder*>(user_data);
  if (!holder || !holder->ctx) return GST_PAD_PROBE_OK;
  auto* ctx = holder->ctx.get();
  const gint64 now = g_get_monotonic_time();
  if (ctx->last_us) {
    ctx->last_us->store(now, std::memory_order_relaxed);
  }
  if (ctx->parsed_count) {
    ctx->parsed_count->fetch_add(1, std::memory_order_relaxed);
  }
  bool expected = false;
  if (ctx->first_seen.compare_exchange_strong(expected, true)) {
    auto* n = new FirstFrameNotify{ctx->self, ctx->camera_id, ctx->generation};
    g_idle_add(&SessionManager::FirstFrameIdleCb, n);
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn SessionManager::HealthBufProbe(GstPad* /*pad*/,
                                                 GstPadProbeInfo* info,
                                                 gpointer user_data) {
  auto* holder = static_cast<HealthProbeHolder*>(user_data);
  if (!holder || !holder->ctx) return GST_PAD_PROBE_OK;
  auto* ctx = holder->ctx.get();
  guint n = 1;
  if (info && (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST)) {
    GstBufferList* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    n = list ? gst_buffer_list_length(list) : 0;
    if (n == 0) return GST_PAD_PROBE_OK;
  }
  if (ctx->count) ctx->count->fetch_add(n, std::memory_order_relaxed);
  if (ctx->last_us) {
    ctx->last_us->store(g_get_monotonic_time(), std::memory_order_relaxed);
  }
  return GST_PAD_PROBE_OK;
}

void SessionManager::AttachHealthProbe(
    std::string camera_id, GstPad* pad,
    std::shared_ptr<std::atomic<guint64>> count,
    std::shared_ptr<std::atomic<gint64>> last_us) {
  if (!pad || !count || !last_us) return;
  auto ctx = std::make_shared<HealthProbeCtx>();
  ctx->count = std::move(count);
  ctx->last_us = std::move(last_us);
  /* holder + destroy notify: the context cannot dangle while mounted */
  auto* holder = new HealthProbeHolder{ctx};
  gulong id = gst_pad_add_probe(
      pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                   GST_PAD_PROBE_TYPE_BUFFER_LIST),
      &SessionManager::HealthBufProbe, holder,
      &SessionManager::HealthProbeHolderFree);
  PadProbeHandle h;
  h.pad = GST_PAD(gst_object_ref(pad));
  h.id = id;
  h.keep_alive = ctx;
  health_probes_by_camera_[camera_id].push_back(std::move(h));
}

void SessionManager::ClearHealthProbes(const std::string& camera_id) {
  auto it = health_probes_by_camera_.find(camera_id);
  if (it == health_probes_by_camera_.end()) return;
  for (auto& h : it->second) {
    if (h.pad && h.id) gst_pad_remove_probe(h.pad, h.id);
    if (h.pad) gst_object_unref(h.pad);
  }
  health_probes_by_camera_.erase(it);
}

void SessionManager::RegisterPeerRtpProbe(const std::string& session_id,
                                          GstElement* pay) {
  if (!registry_ || !pay || session_id.empty()) return;
  UnregisterPeerHealth(session_id);
  auto snap = std::make_shared<pipeline::RuntimeRegistry::PeerHealthSnapshot>();
  registry_->webrtc_peer_health[session_id] = snap;
  GstPad* src = gst_element_get_static_pad(pay, "src");
  if (!src) return;
  auto ctx = std::make_shared<HealthProbeCtx>();
  ctx->count = snap->rtp_out.count;
  ctx->last_us = snap->rtp_out.last_us;
  /* holder + destroy notify: the ctx is guaranteed alive for every callback
   * execution — no raw ctx.get() as probe user_data anywhere. */
  auto* holder = new HealthProbeHolder{ctx};
  gulong id = gst_pad_add_probe(
      src,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                   GST_PAD_PROBE_TYPE_BUFFER_LIST),
      &SessionManager::HealthBufProbe, holder,
      &SessionManager::HealthProbeHolderFree);
  PadProbeHandle h;
  h.pad = src;  // take ownership of get_static_pad ref
  h.id = id;
  h.keep_alive = ctx;
  peer_rtp_probes_[session_id] = std::move(h);
}

void SessionManager::UnregisterPeerHealth(const std::string& session_id) {
  auto it = peer_rtp_probes_.find(session_id);
  if (it != peer_rtp_probes_.end()) {
    if (it->second.pad && it->second.id) {
      gst_pad_remove_probe(it->second.pad, it->second.id);
    }
    if (it->second.pad) gst_object_unref(it->second.pad);
    peer_rtp_probes_.erase(it);
  }
  if (registry_) registry_->webrtc_peer_health.erase(session_id);
}

void SessionManager::TickHealthPrint() {
  if (!registry_) return;
  gint64 now = g_get_monotonic_time();
  if (health_last_print_us_ != 0 &&
      (now - health_last_print_us_) < 2 * G_TIME_SPAN_SECOND) {
    return;
  }
  health_last_print_us_ = now;
  for (const auto& kv : registry_->webrtc_camera_health) {
    const auto& cam = kv.first;
    const auto& h = kv.second;
    if (!h) continue;
    auto age_ms = [&](const auto& ctr) -> gint64 {
      gint64 t = ctr.last_us->load(std::memory_order_relaxed);
      return t > 0 ? (now - t) / 1000 : -1;
    };
    std::cerr << "[HEALTH] cam=" << cam
              << " raw=" << h->raw_in.count->load()
              << " enc_in=" << h->encoder_in.count->load()
              << " enc_out=" << h->encoder_out.count->load()
              << " parsed=" << h->parsed_out.count->load()
              << " age_ms raw/in/out/parsed="
              << age_ms(h->raw_in) << "/" << age_ms(h->encoder_in) << "/"
              << age_ms(h->encoder_out) << "/" << age_ms(h->parsed_out)
              << " postq_ov=" << h->postq_overrun->load()
              << std::endl;
  }
  for (const auto& kv : registry_->webrtc_peer_health) {
    const auto& sid = kv.first;
    const auto& p = kv.second;
    if (!p) continue;
    gint64 t = p->rtp_out.last_us->load(std::memory_order_relaxed);
    gint64 age = t > 0 ? (now - t) / 1000 : -1;
    gint64 st = p->stats_out.last_us->load(std::memory_order_relaxed);
    gint64 stats_age = st > 0 ? (now - st) / 1000 : -1;
    std::cerr << "[HEALTH] rtp session=" << sid
              << " rtp_out=" << p->rtp_out.count->load()
              << " age_ms=" << age
              << " stats_pkts=" << p->stats_out.count->load()
              << " stats_age_ms=" << stats_age << std::endl;
  }
}

GstElement* SessionManager::BuildEncoderChain(std::string camera_id) {
  if (!registry_ || !pipeline_) return nullptr;

  const char* inject = std::getenv("AIBOX_R1B_FAIL_INJECT");
  if (inject && std::strcmp(inject, "create") == 0) {
    std::cerr << "[CP05-R1B] FAIL_INJECT=create for " << camera_id << std::endl;
    return nullptr;
  }

  auto tee_perm_it = registry_->webrtc_stub_tee_by_camera.find(camera_id);
  if (tee_perm_it == registry_->webrtc_stub_tee_by_camera.end() ||
      !tee_perm_it->second) {
    std::cerr << "[SessionManager] BuildEncoderChain: no tee_perm for "
              << camera_id << std::endl;
    return nullptr;
  }
  GstElement* tee_perm = tee_perm_it->second;

  const int gen = ++enc_rebuild_gen_;
  {
    std::lock_guard<std::mutex> lock(generation_mu_);
    encoder_generation_by_camera_[camera_id] = gen;
  }
  const std::string sfx = camera_id + "_g" + std::to_string(gen);

  std::string err;
  auto make = [&](const char* factory, const std::string& name) -> GstElement* {
    GstElement* e = gst_element_factory_make(factory, name.c_str());
    if (!e) err = std::string(factory) + " create failed";
    return e;
  };

  GstElement* bin = gst_bin_new(("encbin_webrtc_" + sfx).c_str());
  GstElement* q = make("queue", "q_webrtc_" + sfx);
  GstElement* vscale = make("videoscale", "vscale_webrtc_" + sfx);
  GstElement* capsflt = make("capsfilter", "caps_webrtc_" + sfx);
  GstElement* enc = make("v4l2h264enc", "enc_webrtc_" + sfx);
  GstElement* parse = make("h264parse", "parse_webrtc_" + sfx);
  // Output tee + keepalive fakesink MUST live on the top-level pipeline so
  // WebRtcSession::DoInit can gst_pad_link(tee.src, session.queue.sink) without
  // GST_PAD_LINK_WRONG_HIERARCHY (-1). Bin only owns the encoder chain.
  GstElement* tee = make("tee", "tee_webrtc_" + sfx);
  GstElement* fsink = make("fakesink", "fakesink_enc_webrtc_" + sfx);
  if (!bin || !q || !vscale || !capsflt || !enc || !parse || !tee || !fsink) {
    std::cerr << "[SessionManager] BuildEncoderChain elem fail: " << err
              << std::endl;
    if (bin) gst_object_unref(bin);
    if (q) gst_object_unref(q);
    if (vscale) gst_object_unref(vscale);
    if (capsflt) gst_object_unref(capsflt);
    if (enc) gst_object_unref(enc);
    if (parse) gst_object_unref(parse);
    if (tee) gst_object_unref(tee);
    if (fsink) gst_object_unref(fsink);
    return nullptr;
  }

  g_object_set(tee, "allow-not-linked", TRUE, nullptr);
  g_object_set(q,
               "max-size-buffers", (guint)4,
               "max-size-bytes", (guint)0,
               "max-size-time", (guint64)0,
               "leaky", 2,
               nullptr);
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
      "format", G_TYPE_STRING, "NV12", nullptr);
  g_object_set(capsflt, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(parse, "config-interval", (gint)-1, nullptr);
  g_object_set(fsink, "sync", FALSE, "async", FALSE, nullptr);

  // A/B/C/D single-variable encoder I/O + optional non-leaky post-encoder queue.
  // A=auto noq (default), B=QTI4/5 noq, C=auto+q, D=QTI4/5+q.
  const char* io_mode_env = std::getenv("AIBOX_ENC_IO_MODE");
  const std::string io_mode = io_mode_env ? io_mode_env : "auto";
  const bool use_qti45 =
      (io_mode == "dmabuf45" || io_mode == "4/5" || io_mode == "qti45" ||
       io_mode == "B" || io_mode == "D" || io_mode == "b" || io_mode == "d");
  const char* postq_env = std::getenv("AIBOX_ENC_POSTQ");
  const bool use_postq =
      postq_env && (std::strcmp(postq_env, "1") == 0 ||
                    std::strcmp(postq_env, "true") == 0 ||
                    std::strcmp(postq_env, "C") == 0 ||
                    std::strcmp(postq_env, "D") == 0 ||
                    std::strcmp(postq_env, "c") == 0 ||
                    std::strcmp(postq_env, "d") == 0);
  // Also allow AIBOX_ENC_VARIANT=A|B|C|D shorthand.
  if (const char* var = std::getenv("AIBOX_ENC_VARIANT")) {
    // handled below via local overrides
    (void)var;
  }
  bool force_qti = use_qti45;
  bool force_postq = use_postq;
  if (const char* var = std::getenv("AIBOX_ENC_VARIANT")) {
    if (var[0] == 'B' || var[0] == 'b') { force_qti = true; force_postq = false; }
    else if (var[0] == 'C' || var[0] == 'c') { force_qti = false; force_postq = true; }
    else if (var[0] == 'D' || var[0] == 'd') { force_qti = true; force_postq = true; }
    else { force_qti = false; force_postq = false; }  // A
  }

  if (force_qti) {
    std::string e1, e2;
    const bool ok1 = pipeline::SetEnumPropertyByNick(
        enc, "capture-io-mode", "dmabuf", &e1);
    const bool ok2 = pipeline::SetEnumPropertyByNick(
        enc, "output-io-mode", "dmabuf-import", &e2);
    gint cap_v = -1, out_v = -1;
    g_object_get(enc, "capture-io-mode", &cap_v, "output-io-mode", &out_v,
                 nullptr);
    std::cerr << "[ENC-CFG] cam=" << camera_id
              << " io=QTI4/5 set_ok=" << ok1 << "/" << ok2
              << " readback capture=" << cap_v << " output=" << out_v;
    if (!ok1) std::cerr << " err1=" << e1;
    if (!ok2) std::cerr << " err2=" << e2;
    std::cerr << std::endl;
  } else {
    std::cerr << "[ENC-CFG] cam=" << camera_id << " io=auto" << std::endl;
  }

  GstElement* postq = nullptr;
  if (force_postq) {
    postq = make("queue", "postq_webrtc_" + sfx);
    if (!postq) {
      std::cerr << "[SessionManager] BuildEncoderChain postq create failed\n";
      if (bin) gst_object_unref(bin);
      if (q) gst_object_unref(q);
      if (vscale) gst_object_unref(vscale);
      if (capsflt) gst_object_unref(capsflt);
      if (enc) gst_object_unref(enc);
      if (parse) gst_object_unref(parse);
      if (tee) gst_object_unref(tee);
      if (fsink) gst_object_unref(fsink);
      return nullptr;
    }
    // Non-leaky by default (leaky risks dropping H.264 refs → browser corruption).
    g_object_set(postq,
                 "max-size-buffers", (guint)0,
                 "max-size-bytes", (guint)0,
                 "max-size-time", (guint64)(100 * GST_MSECOND),
                 "leaky", 0,
                 nullptr);
    std::cerr << "[ENC-CFG] cam=" << camera_id
              << " postq=on max-size-time=100ms leaky=0" << std::endl;
  } else {
    std::cerr << "[ENC-CFG] cam=" << camera_id << " postq=off" << std::endl;
  }

  if (postq) {
    gst_bin_add_many(GST_BIN(bin), q, vscale, capsflt, enc, postq, parse,
                     nullptr);
  } else {
    gst_bin_add_many(GST_BIN(bin), q, vscale, capsflt, enc, parse, nullptr);
  }

  auto bounded_remove = [&](GstElement* e) {
    if (!e) return;
    gst_element_set_locked_state(e, TRUE);
    gst_element_set_state(e, GST_STATE_NULL);
    gst_element_get_state(e, nullptr, nullptr, 200 * GST_MSECOND);
    if (GST_OBJECT_PARENT(e) == GST_OBJECT(pipeline_)) {
      gst_bin_remove(GST_BIN(pipeline_), e);
    } else {
      gst_object_unref(e);
    }
  };

  GstPad* perm_src = nullptr;
  GstPad* tee_ka_src = nullptr;
  auto fail_cleanup = [&]() {
    if (perm_src) {
      GstPad* peer = gst_pad_get_peer(perm_src);
      if (peer) {
        gst_pad_unlink(perm_src, peer);
        gst_object_unref(peer);
      }
      gst_element_release_request_pad(tee_perm, perm_src);
      gst_object_unref(perm_src);
      perm_src = nullptr;
    }
    GstPad* bin_src = gst_element_get_static_pad(bin, "src");
    if (bin_src) {
      GstPad* peer = gst_pad_get_peer(bin_src);
      if (peer) {
        gst_pad_unlink(bin_src, peer);
        gst_object_unref(peer);
      }
      gst_object_unref(bin_src);
    }
    if (tee_ka_src) {
      GstPad* peer = gst_pad_get_peer(tee_ka_src);
      if (peer) {
        gst_pad_unlink(tee_ka_src, peer);
        gst_object_unref(peer);
      }
      gst_element_release_request_pad(tee, tee_ka_src);
      gst_object_unref(tee_ka_src);
      tee_ka_src = nullptr;
    }
    bounded_remove(fsink);
    bounded_remove(tee);
    bounded_remove(bin);
  };

  if (inject && std::strcmp(inject, "link") == 0) {
    std::cerr << "[CP05-R1B] FAIL_INJECT=link for " << camera_id << std::endl;
    fail_cleanup();
    return nullptr;
  }

  if (!gst_element_link_many(q, vscale, capsflt, enc, nullptr)) {
    std::cerr << "[SessionManager] BuildEncoderChain link pre-enc failed\n";
    fail_cleanup();
    return nullptr;
  }
  if (postq) {
    if (!gst_element_link_many(enc, postq, parse, nullptr)) {
      std::cerr << "[SessionManager] BuildEncoderChain link enc→postq→parse failed\n";
      fail_cleanup();
      return nullptr;
    }
  } else if (!gst_element_link(enc, parse)) {
    std::cerr << "[SessionManager] BuildEncoderChain link enc→parse failed\n";
    fail_cleanup();
    return nullptr;
  }

  GstPad* q_sink = gst_element_get_static_pad(q, "sink");
  GstPad* parse_src_ghost = gst_element_get_static_pad(parse, "src");
  if (!q_sink || !parse_src_ghost) {
    if (q_sink) gst_object_unref(q_sink);
    if (parse_src_ghost) gst_object_unref(parse_src_ghost);
    fail_cleanup();
    return nullptr;
  }
  GstPad* ghost_sink = gst_ghost_pad_new("sink", q_sink);
  GstPad* ghost_src = gst_ghost_pad_new("src", parse_src_ghost);
  gst_object_unref(q_sink);
  gst_object_unref(parse_src_ghost);
  if (!ghost_sink || !gst_element_add_pad(bin, ghost_sink)) {
    std::cerr << "[SessionManager] BuildEncoderChain ghost sink failed\n";
    if (ghost_sink) gst_object_unref(ghost_sink);
    if (ghost_src) gst_object_unref(ghost_src);
    fail_cleanup();
    return nullptr;
  }
  if (!ghost_src || !gst_element_add_pad(bin, ghost_src)) {
    std::cerr << "[SessionManager] BuildEncoderChain ghost src failed\n";
    if (ghost_src) gst_object_unref(ghost_src);
    fail_cleanup();
    return nullptr;
  }

  gst_bin_add(GST_BIN(pipeline_), bin);
  gst_bin_add_many(GST_BIN(pipeline_), tee, fsink, nullptr);

  // OUT frame counter: the WebRTC encoder chain only exists here, so register
  // the per-camera counter on the encoder src pad from this point. The "Frame
  // Count Summary" OUT column reads it (real encoded frames, not RTP packets).
  pipeline::AttachFrameCountOutProbe(enc, camera_id, registry_);

  GstPad* bin_src = gst_element_get_static_pad(bin, "src");
  GstPad* tee_sink = gst_element_get_static_pad(tee, "sink");
  if (!bin_src || !tee_sink ||
      gst_pad_link(bin_src, tee_sink) != GST_PAD_LINK_OK) {
    std::cerr << "[SessionManager] BuildEncoderChain bin.src→tee failed\n";
    if (bin_src) gst_object_unref(bin_src);
    if (tee_sink) gst_object_unref(tee_sink);
    fail_cleanup();
    return nullptr;
  }
  gst_object_unref(bin_src);
  gst_object_unref(tee_sink);

  tee_ka_src = gst_element_request_pad_simple(tee, "src_%u");
  GstPad* fsink_pad = gst_element_get_static_pad(fsink, "sink");
  if (!tee_ka_src || !fsink_pad ||
      gst_pad_link(tee_ka_src, fsink_pad) != GST_PAD_LINK_OK) {
    std::cerr << "[SessionManager] BuildEncoderChain tee→fakesink failed\n";
    if (fsink_pad) gst_object_unref(fsink_pad);
    fail_cleanup();
    return nullptr;
  }
  gst_object_unref(fsink_pad);
  gst_object_unref(tee_ka_src);
  tee_ka_src = nullptr;

  perm_src = gst_element_request_pad_simple(tee_perm, "src_%u");
  if (!perm_src) {
    std::cerr << "[SessionManager] BuildEncoderChain tee_perm pad failed\n";
    fail_cleanup();
    return nullptr;
  }

  // Align with R1A: link then sync; no BLOCK pad probe (unverified semantics
  // and can stall Venus preroll). READY remains first-frame gated.
  if (gst_pad_link(perm_src, ghost_sink) != GST_PAD_LINK_OK) {
    std::cerr << "[SessionManager] BuildEncoderChain tee_perm→bin failed\n";
    fail_cleanup();
    return nullptr;
  }

  auto health = std::make_shared<pipeline::RuntimeRegistry::CameraHealthSnapshot>();
  registry_->webrtc_camera_health[camera_id] = health;

  auto probe_ctx = std::make_shared<EncProbeCtx>();
  probe_ctx->self = this;
  probe_ctx->camera_id = camera_id;
  probe_ctx->generation = gen;
  probe_ctx->last_us =
      std::make_shared<std::atomic<gint64>>(g_get_monotonic_time());
  probe_ctx->parsed_count = health->parsed_out.count;
  // Keep watchdog last_us alias on parsed_out for TickWatchdog compatibility.
  health->parsed_out.last_us = probe_ctx->last_us;

  ClearHealthProbes(camera_id);
  // raw_in: encoder queue sink (frames entering encoder branch)
  if (GstPad* q_sink_p = gst_element_get_static_pad(q, "sink")) {
    AttachHealthProbe(camera_id, q_sink_p, health->raw_in.count,
                      health->raw_in.last_us);
    gst_object_unref(q_sink_p);
  }
  if (GstPad* enc_sink = gst_element_get_static_pad(enc, "sink")) {
    AttachHealthProbe(camera_id, enc_sink, health->encoder_in.count,
                      health->encoder_in.last_us);
    gst_object_unref(enc_sink);
  }
  if (GstPad* enc_src = gst_element_get_static_pad(enc, "src")) {
    AttachHealthProbe(camera_id, enc_src, health->encoder_out.count,
                      health->encoder_out.last_us);
    gst_object_unref(enc_src);
  }

  GstPad* parse_src = gst_element_get_static_pad(parse, "src");
  gulong probe_id = 0;
  if (parse_src) {
    /* holder + destroy notify: ctx survives until the probe is removed */
    auto* holder = new EncProbeHolder{probe_ctx};
    probe_id = gst_pad_add_probe(
        parse_src, GST_PAD_PROBE_TYPE_BUFFER,
        &SessionManager::EncOutProbe, holder,
        &SessionManager::EncProbeHolderFree);
  }

  // Do NOT block the GLib main loop with a multi-second get_state wait:
  // SIGTERM and first-frame timeouts must stay responsive. FAIL_INJECT=state
  // still forces failure; otherwise sync_ok alone gates structure-ready.
  const gboolean sync_bin = gst_element_sync_state_with_parent(bin);
  const gboolean sync_tee = gst_element_sync_state_with_parent(tee);
  const gboolean sync_fs = gst_element_sync_state_with_parent(fsink);
  bool state_fail = false;
  if (inject && std::strcmp(inject, "state") == 0) {
    std::cerr << "[CP05-R1B] FAIL_INJECT=state for " << camera_id << std::endl;
    state_fail = true;
  } else if (!sync_bin || !sync_tee || !sync_fs) {
    state_fail = true;
  }

  if (state_fail) {
    std::cerr << "[SessionManager] BuildEncoderChain state failed cam="
              << camera_id << " sync_bin=" << sync_bin
              << " sync_tee=" << sync_tee << " sync_fs=" << sync_fs
              << std::endl;
    if (probe_id && parse_src) gst_pad_remove_probe(parse_src, probe_id);
    if (parse_src) gst_object_unref(parse_src);
    ClearHealthProbes(camera_id);
    registry_->webrtc_camera_health.erase(camera_id);
    fail_cleanup();
    return nullptr;
  }

  registry_->webrtc_encoder_bin_by_camera[camera_id] = bin;
  registry_->webrtc_tee_by_camera[camera_id] = tee;
  registry_->webrtc_encoder_by_camera[camera_id] = enc;
  registry_->webrtc_h264parse_by_camera[camera_id] = parse;
  registry_->webrtc_capsfilter_by_camera[camera_id] = capsflt;
  registry_->webrtc_videoscale_by_camera[camera_id] = vscale;
  registry_->webrtc_preenc_queue_by_camera[camera_id] = q;
  registry_->webrtc_encfsink_by_camera[camera_id] = fsink;
  registry_->webrtc_teeperm_encpad_by_camera[camera_id] = perm_src;
  registry_->webrtc_enc_last_buffer_us[camera_id] = probe_ctx->last_us;
  registry_->webrtc_enc_probe_id_by_camera[camera_id] = probe_id;
  registry_->webrtc_enc_probe_pad_by_camera[camera_id] = parse_src;
  {
    std::lock_guard<std::mutex> lock(mu_);
    enc_probe_ctx_by_camera_[camera_id] = probe_ctx;
    webrtc_tees_[camera_id] = tee;
  }

  std::cerr << "[SessionManager] P3b enc structure ready: " << camera_id
            << " encbin(q→vscale→caps→enc→parse) ghost→tee(top) gen=" << gen
            << " (await first frame for READY)\n";
  return tee;
}

void SessionManager::TeardownEncoderChain(std::string camera_id) {
  if (!registry_ || !pipeline_) return;

  std::cout << "[CP05-TD] cam=" << camera_id << " stage=begin" << std::endl;

  // Remove out-frame probe BEFORE destroying EncProbeCtx (UAF otherwise).
  std::cout << "[CP05-TD] cam=" << camera_id
            << " stage=remove_enc_probe enter" << std::endl;
  auto pid_it = registry_->webrtc_enc_probe_id_by_camera.find(camera_id);
  auto ppad_it = registry_->webrtc_enc_probe_pad_by_camera.find(camera_id);
  if (pid_it != registry_->webrtc_enc_probe_id_by_camera.end() &&
      ppad_it != registry_->webrtc_enc_probe_pad_by_camera.end() &&
      ppad_it->second && pid_it->second) {
    gst_pad_remove_probe(ppad_it->second, pid_it->second);
  }
  if (ppad_it != registry_->webrtc_enc_probe_pad_by_camera.end() &&
      ppad_it->second) {
    gst_object_unref(ppad_it->second);
  }
  registry_->webrtc_enc_probe_id_by_camera.erase(camera_id);
  registry_->webrtc_enc_probe_pad_by_camera.erase(camera_id);
  std::cout << "[CP05-TD] cam=" << camera_id
            << " stage=remove_enc_probe exit" << std::endl;
  ClearHealthProbes(camera_id);
  registry_->webrtc_camera_health.erase(camera_id);

  {
    std::lock_guard<std::mutex> lock(mu_);
    enc_probe_ctx_by_camera_.erase(camera_id);
  }

  // Release tee_perm → enc pad.
  std::cout << "[CP05-TD] cam=" << camera_id
            << " stage=unlink_tee_perm enter" << std::endl;
  auto tee_perm_it = registry_->webrtc_stub_tee_by_camera.find(camera_id);
  auto encpad_it = registry_->webrtc_teeperm_encpad_by_camera.find(camera_id);
  if (tee_perm_it != registry_->webrtc_stub_tee_by_camera.end() &&
      tee_perm_it->second &&
      encpad_it != registry_->webrtc_teeperm_encpad_by_camera.end() &&
      encpad_it->second) {
    GstPad* peer = gst_pad_get_peer(encpad_it->second);
    if (peer) {
      gst_pad_unlink(encpad_it->second, peer);
      gst_object_unref(peer);
    }
    gst_element_release_request_pad(tee_perm_it->second, encpad_it->second);
    gst_object_unref(encpad_it->second);
  }
  registry_->webrtc_teeperm_encpad_by_camera.erase(camera_id);
  std::cout << "[CP05-TD] cam=" << camera_id
            << " stage=unlink_tee_perm exit" << std::endl;

  auto take = [&](auto& m) -> GstElement* {
    auto it = m.find(camera_id);
    if (it == m.end()) return nullptr;
    GstElement* e = it->second;
    m.erase(it);
    return e;
  };

  GstElement* bin = take(registry_->webrtc_encoder_bin_by_camera);
  GstElement* fsink = take(registry_->webrtc_encfsink_by_camera);
  GstElement* tee = take(registry_->webrtc_tee_by_camera);
  take(registry_->webrtc_h264parse_by_camera);
  take(registry_->webrtc_encoder_by_camera);
  take(registry_->webrtc_capsfilter_by_camera);
  take(registry_->webrtc_videoscale_by_camera);
  take(registry_->webrtc_preenc_queue_by_camera);
  registry_->webrtc_enc_last_buffer_us.erase(camera_id);

  auto bounded_remove = [&](GstElement* e) {
    if (!e) return;
    std::cout << "[CP05-TD] cam=" << camera_id << " stage=set_null enter"
              << std::endl;
    gst_element_set_locked_state(e, TRUE);
    gst_element_set_state(e, GST_STATE_NULL);
    gst_element_get_state(e, nullptr, nullptr, 500 * GST_MSECOND);
    std::cout << "[CP05-TD] cam=" << camera_id << " stage=set_null exit"
              << std::endl;
    if (GST_OBJECT_PARENT(e) == GST_OBJECT(pipeline_)) {
      gst_bin_remove(GST_BIN(pipeline_), e);
    }
  };

  // Unlink bin.src(ghost) → tee.sink before removing top-level fanout.
  if (bin && tee) {
    GstPad* bin_src = gst_element_get_static_pad(bin, "src");
    GstPad* tee_sink = gst_element_get_static_pad(tee, "sink");
    if (bin_src && tee_sink && gst_pad_is_linked(bin_src)) {
      gst_pad_unlink(bin_src, tee_sink);
    }
    if (bin_src) gst_object_unref(bin_src);
    if (tee_sink) gst_object_unref(tee_sink);
  }

  // Unlink keepalive tee.src → fakesink and release the request pad.
  if (tee && fsink) {
    GstPad* fsink_sink = gst_element_get_static_pad(fsink, "sink");
    if (fsink_sink) {
      GstPad* tee_src = gst_pad_get_peer(fsink_sink);
      if (tee_src) {
        gst_pad_unlink(tee_src, fsink_sink);
        gst_element_release_request_pad(tee, tee_src);
        gst_object_unref(tee_src);
      }
      gst_object_unref(fsink_sink);
    }
  }

  bounded_remove(fsink);
  bounded_remove(tee);
  bounded_remove(bin);

  {
    std::lock_guard<std::mutex> lock(mu_);
    webrtc_tees_[camera_id] = nullptr;
  }
  std::cout << "[CP05-TD] cam=" << camera_id << " stage=done" << std::endl;
}

bool SessionManager::RebuildWebrtcEncoder(std::string camera_id) {
  // CP05-R1A: live hot-rebuild disabled. Stall recovery is runner-owned.
  std::cerr << "[CP05-R1A] RebuildWebrtcEncoder REFUSED cam=" << camera_id
            << " (live hot-rebuild disabled; runner owns restart)\n";
  return false;
}

bool SessionManager::AllowForceKeyUnit(const std::string& camera_id) {
  std::lock_guard<std::mutex> lk(selfheal_mu_);
  gint64 now = g_get_monotonic_time();
  auto it = fku_last_us_.find(camera_id);
  if (it != fku_last_us_.end() &&
      (now - it->second) < 2 * G_TIME_SPAN_SECOND) {
    return false;
  }
  fku_last_us_[camera_id] = now;
  return true;
}

bool SessionManager::ConsumeCircuitBreakerTrip() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!circuit_breaker_pending_notify_) return false;
  circuit_breaker_pending_notify_ = false;
  return true;
}

bool SessionManager::IsEncoderBuildInProgress() const {
  std::lock_guard<std::mutex> lock(mu_);
  return encoder_build_in_progress_;
}

void SessionManager::TickWatchdog() {
  // RemoveSession also takes mu_; collect first, reclaim after unlock.
  std::vector<std::pair<std::string, const char*>> reclaim;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!registry_ || !initialized_) return;
    TickHealthPrint();
    if (circuit_breaker_tripped_) return;
    // Same as Gate 1: serial first-frame must not share the watchdog tick with
    // stall accounting or peer reclaim. Extra work here coincided with cam-004
    // never getting a first encoded buffer (raw_in=0) on cold Start All.
    if (encoder_build_in_progress_) return;

    gint64 now = g_get_monotonic_time();
    if (watchdog_last_tick_us_ != 0 &&
        (now - watchdog_last_tick_us_) < 1 * G_TIME_SPAN_SECOND) {
      return;
    }
    watchdog_last_tick_us_ = now;

    for (auto& kv : sessions_) {
      if (!kv.second) continue;
      kv.second->PollOutboundStats();
      auto peer = registry_->webrtc_peer_health.find(kv.first);
      if (peer != registry_->webrtc_peer_health.end() && peer->second) {
        peer->second->stats_out.count->store(
            kv.second->StatsPacketsSent(), std::memory_order_relaxed);
        peer->second->stats_out.last_us->store(
            kv.second->StatsLastUs(), std::memory_order_relaxed);
      }
    }

    std::unordered_map<std::string, int> live_by_cam;
    for (auto& kv : sessions_) {
      if (!kv.second) continue;
      if (kv.second->GetState() == WebRtcSession::State::kClosed) continue;
      live_by_cam[kv.second->GetCameraId()]++;
    }

    std::vector<std::string> live_ready;
    for (const auto& kv : encoder_state_by_camera_) {
      if (kv.second != EncoderState::kReady) continue;
      if (live_by_cam[kv.first] <= 0) continue;
      live_ready.push_back(kv.first);
    }

    constexpr gint64 kStallUs = 5 * G_TIME_SPAN_SECOND;
    constexpr gint64 kMajorityStallUs = 30 * G_TIME_SPAN_SECOND;

    std::set<std::string> cur(live_ready.begin(), live_ready.end());
    const bool topo_changing =
        !closing_sessions_.empty() ||
        !pending_replacement_by_camera_.empty() ||
        cur != last_live_set_;
    if (topo_changing) {
      if (cur != last_live_set_) {
        std::cerr << "[Gate3] suppress trip (topology settling) live="
                  << cur.size() << " prev=" << last_live_set_.size()
                  << " closing=" << closing_sessions_.size()
                  << " pending_repl=" << pending_replacement_by_camera_.size()
                  << std::endl;
      }
      topology_settle_until_us_ = now + kStallUs;
      stall_since_us_.clear();
    }
    last_live_set_ = cur;
    const bool suppress_trip =
        topo_changing || (now < topology_settle_until_us_);

    std::vector<std::string> stalled;
    if (!live_ready.empty()) {
      for (const auto& cam : live_ready) {
        auto last_it = registry_->webrtc_enc_last_buffer_us.find(cam);
        if (last_it == registry_->webrtc_enc_last_buffer_us.end() ||
            !last_it->second) {
          continue;
        }
        gint64 last = last_it->second->load(std::memory_order_relaxed);
        if (last <= 0) continue;
        if ((now - last) <= kStallUs) continue;
        stalled.push_back(cam);
      }
    }

    {
      std::unordered_map<std::string, gint64> next_since;
      for (const auto& cam : stalled) {
        auto it = stall_since_us_.find(cam);
        next_since[cam] = (it == stall_since_us_.end()) ? now : it->second;
      }
      stall_since_us_.swap(next_since);
    }

    const int n_live = static_cast<int>(live_ready.size());
    const int n_stall = static_cast<int>(stalled.size());
    bool trip = false;
    const char* trip_why = nullptr;
    if (suppress_trip) {
      // Planned Close / REPLACE / encoder-adjacent topology: never trip.
    } else if (n_live == 0) {
      // Close All / no READY peer: never trip (0==0 would false-fire).
    } else if (n_stall == n_live) {
      trip = true;
      trip_why = "all-live";
    } else if (n_live >= 2 && n_stall > 0) {
      const int half = (n_live + 1) / 2;
      int long_stall = 0;
      for (const auto& cam : stalled) {
        if ((now - stall_since_us_[cam]) >= kMajorityStallUs) ++long_stall;
      }
      if (long_stall >= half) {
        trip = true;
        trip_why = "majority-30s";
      }
    }

    for (const auto& cam : stalled) {
      bool warn = false;
      {
        std::lock_guard<std::mutex> lk(selfheal_mu_);
        auto it = selfheal_last_us_.find(cam);
        if (it == selfheal_last_us_.end() ||
            (now - it->second) >= 10 * G_TIME_SPAN_SECOND) {
          selfheal_last_us_[cam] = now;
          warn = true;
        }
      }
      if (warn) {
        std::cerr << "[WARN] encoder stall cam=" << cam
                  << " others-live=" << (n_live - n_stall)
                  << " stalled=" << n_stall << "/" << n_live
                  << " — no live rebuild" << std::endl;
      }
    }

    if (trip && !circuit_breaker_tripped_) {
      circuit_breaker_tripped_ = true;
      circuit_breaker_pending_notify_ = true;
      std::cerr << "[ERROR] Preview gate3 " << (trip_why ? trip_why : "?")
                << " stall " << n_stall << "/" << n_live
                << " — circuit-breaker open — exiting for runner retry "
                   "(no VPU reset, no hot rebuild)"
                << std::endl;
    }

    if (!circuit_breaker_tripped_) {
      CollectDeadPeersLocked(now, reclaim);
    }
  }

  for (const auto& r : reclaim) {
    std::cerr << "[SessionManager] RECLAIM sid=" << r.first
              << " why=" << (r.second ? r.second : "?") << std::endl;
    RemoveSession(r.first);
  }
}

void SessionManager::CollectDeadPeersLocked(
    gint64 now, std::vector<std::pair<std::string, const char*>>& out) {
  if (shutting_down_ || !registry_) return;

  // Encoder still feeding this camera: peer is dead, not a shared Venus stall.
  // Only this sid. Never "newer sid on same cam → kill old".
  constexpr gint64 kParsedFreshUs = 2 * G_TIME_SPAN_SECOND;
  constexpr gint64 kRtpStaleUs = 8 * G_TIME_SPAN_SECOND;
  constexpr gint64 kRtpNeverUs = 15 * G_TIME_SPAN_SECOND;

  for (const auto& kv : sessions_) {
    if (!kv.second) continue;
    const auto st = kv.second->GetState();
    if (st == WebRtcSession::State::kClosed) continue;
    // Serial encoder build leaves later cams in kPending; do not arm rtp_never
    // from session.new or last-cam ICE would lose the race.
    if (st == WebRtcSession::State::kPending) continue;
    const std::string& sid = kv.first;
    const std::string& cam = kv.second->GetCameraId();

    auto enc = encoder_state_by_camera_.find(cam);
    if (enc == encoder_state_by_camera_.end() ||
        enc->second != EncoderState::kReady) {
      continue;
    }

    auto cam_h = registry_->webrtc_camera_health.find(cam);
    if (cam_h == registry_->webrtc_camera_health.end() || !cam_h->second) {
      continue;
    }
    gint64 parsed_last =
        cam_h->second->parsed_out.last_us->load(std::memory_order_relaxed);
    if (parsed_last <= 0 || (now - parsed_last) > kParsedFreshUs) {
      continue;
    }

    auto& armed = session_armed_us_[sid];
    if (armed <= 0) {
      armed = now;
      continue;
    }

    guint64 rtp_count = 0;
    gint64 rtp_last = 0;
    auto peer = registry_->webrtc_peer_health.find(sid);
    if (peer != registry_->webrtc_peer_health.end() && peer->second) {
      rtp_count = peer->second->rtp_out.count->load(std::memory_order_relaxed);
      rtp_last = peer->second->rtp_out.last_us->load(std::memory_order_relaxed);
    }
    const guint64 stats_count = kv.second->StatsPacketsSent();
    const gint64 stats_last = kv.second->StatsLastUs();
    const bool probe_fresh =
        rtp_last > 0 && (now - rtp_last) <= kRtpStaleUs;
    const bool stats_fresh =
        stats_last > 0 && (now - stats_last) <= kRtpStaleUs;
    // parsed_out is fresh (above). Do not reclaim if either meter still moves.
    // Phase A: BUFFER-only probe lied; get-stats / BUFFER_LIST is the veto.
    if (rtp_count > 0 || stats_count > 0) {
      if (!probe_fresh && !stats_fresh) {
        if (kv.second->IsWaitingIdr()) continue;
        out.emplace_back(sid, "rtp_stale");
      }
    } else if ((now - armed) > kRtpNeverUs) {
      if (kv.second->IsWaitingIdr()) continue;
      out.emplace_back(sid, "rtp_never");
    }
  }
}

bool SessionManager::StartSignaling() {
  adapter::NatsAdapter* nats = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!nats_ || shutting_down_ || signaling_) return false;
    nats = nats_;
  }

  auto signaling = std::make_unique<WebrtcSignaling>(nats);
  const bool ok = signaling->Start(
      [this](const SessionNewPayload& p)   { OnNewSession(p); },
      [this](const SdpPayload& p)          { OnAnswer(p); },
      [this](const IceCandidatePayload& p) { OnIceRemote(p); },
      [this](const SessionClosePayload& p) { OnClose(p); },
      [this](const RenegotiatePayload& p)  { OnRenegotiate(p); });
  if (!ok) return false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_ || signaling_) {
      // Do not destroy while holding mu_: Stop waits for NATS callbacks that
      // may themselves be entering SessionManager.
    } else {
      signaling_ = std::move(signaling);
    }
  }
  if (signaling) {
    signaling->Stop();
    return false;
  }

  std::cout << "[SessionManager] Signaling started" << std::endl;
  return true;
}

void SessionManager::Shutdown() {
  std::unique_ptr<WebrtcSignaling> signaling;
  std::vector<std::string> ids;
  std::vector<std::string> enc_cams;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;
    shutting_down_ = true;
    /* ① forbid new sessions / encoder builds from this point on */
    if (encoder_build_source_) {
      g_source_remove(encoder_build_source_);
      encoder_build_source_ = 0;
    }
    for (auto& kv : first_frame_timeout_by_camera_) {
      if (kv.second) g_source_remove(kv.second);
    }
    first_frame_timeout_by_camera_.clear();
    encoder_build_in_progress_ = false;
    encoder_build_camera_.clear();
    encoder_build_generation_ = 0;
    encoder_build_queue_.clear();
    pending_init_by_camera_.clear();
    pending_replacement_by_camera_.clear();
    first_frame_retry_by_camera_.clear();
    first_frame_soft_extend_by_camera_.clear();
    /* DO NOT clear enc_probe_ctx_by_camera_ here: every established encoder
     * chain owns its ctx and releases it at the END of its own teardown
     * (probes removed first). Pre-clearing here = use-after-free. */
    for (auto& kv : webrtc_tees_) {
      if (kv.second) enc_cams.push_back(kv.first);
    }
    signaling = std::move(signaling_);
    for (auto& kv : sessions_) ids.push_back(kv.first);
  }

  /* ② close the WebRTC private session chains — outside mu_ (NATS
   * unsubscribe waits for active callbacks that may take mu_). */
  if (signaling) signaling->Stop();
  for (auto& id : ids) RemoveSession(id);

  std::unordered_map<std::string, std::unique_ptr<WebRtcSession>> closing;
  {
    std::lock_guard<std::mutex> lock(mu_);
    closing.swap(closing_sessions_);
    sessions_.clear();
  }
  for (auto& kv : closing) {
    if (!kv.second) continue;
    kv.second->ProcessQueue();
    kv.second->ForceFinishTeardown();
  }
  closing.clear();

  /* ③ teardown EVERY established encoder chain while registry_ is still
   * valid — the dynamic encoder chains must not be left to the pipeline
   * NULL phase (pad-mutex deadlock). GStreamer calls run outside mu_. */
  for (auto& cam : enc_cams) {
    if (registry_ && registry_->builder_static_webrtc_cams.count(cam)) {
      // Builder-static chain (e.g. annotated RTSP mode) — NOT SessionManager-
      // owned: tearing it down here while the pipeline is still PLAYING makes
      // the demux push into unlinked branches (not-linked spam). It is
      // released together with the pipeline at the NULL transition.
      std::cout << "[CP05-TD] Shutdown skip static builder chain cam=" << cam
                << " (released at pipeline NULL)" << std::endl;
      continue;
    }
    std::cout << "[CP05-TD] Shutdown teardown begin cam=" << cam << std::endl;
    TeardownEncoderChain(cam);
    std::cout << "[CP05-TD] Shutdown teardown done cam=" << cam << std::endl;
  }

  std::lock_guard<std::mutex> lock(mu_);
  webrtc_tees_.clear();
  registry_ = nullptr;
  initialized_ = false;
  std::cout << "[SessionManager] Shutdown complete" << std::endl;
}

bool SessionManager::HasCamera(const std::string& camera_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return webrtc_tees_.count(camera_id) > 0;
}

int SessionManager::ProcessAllQueues() {
  std::lock_guard<std::mutex> lock(mu_);
  int total = 0;
  // One DoInit per 20ms tick: Start All after encoders are already READY
  // must not drain 9 webrtcbin setups in a single GLib iteration.
  // Close / answer / ICE are never deferred by this budget.
  int init_budget = 1;
  for (auto& kv : sessions_) {
    total += kv.second->ProcessQueue(&init_budget);
  }
  // Gate 3: drain Close ops for sessions already moved out of sessions_.
  for (auto& kv : closing_sessions_) {
    if (kv.second) total += kv.second->ProcessQueue(nullptr);
  }
  DrainPendingReplacementsLocked();
  return total;
}

void SessionManager::OnNewSession(const SessionNewPayload& p) {
  std::string resolved_camera_id = p.camera_id;
  std::vector<std::string> old_sids;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_ || !initialized_) return;
    auto it = webrtc_tees_.find(resolved_camera_id);
    if (it == webrtc_tees_.end() && registry_ && !p.rtsp_url.empty()) {
      const auto rtsp_it = registry_->camera_id_by_rtsp_url.find(p.rtsp_url);
      if (rtsp_it != registry_->camera_id_by_rtsp_url.end()) {
        resolved_camera_id = rtsp_it->second;
        it = webrtc_tees_.find(resolved_camera_id);
      }
    }
    if (it == webrtc_tees_.end()) {
      std::cerr << "[SessionManager] Ignoring session.new for unknown camera: "
                << p.camera_id << " (rtsp=" << p.rtsp_url << ")\n";
      return;
    }
    if (!signaling_ || !signaling_->IsAvailable()) {
      std::cerr << "[SessionManager] NATS unavailable, rejecting new session: "
                << p.session_id << std::endl;
      return;
    }
    if (sessions_.count(p.session_id) > 0 ||
        closing_sessions_.count(p.session_id) > 0) {
      std::cerr << "[SessionManager] Duplicate session_id: "
                << p.session_id << std::endl;
      return;
    }
    for (const auto& kv : sessions_) {
      if (!kv.second) continue;
      if (kv.second->GetCameraId() != resolved_camera_id) continue;
      if (kv.second->GetState() == WebRtcSession::State::kClosed) continue;
      old_sids.push_back(kv.first);
    }
  }

  // One private branch per camera. Tear down leftovers asynchronously, then
  // create the latest generation only after SESSION_CLOSE_DONE.
  for (const auto& old : old_sids) {
    std::cerr << "[SessionManager] REPLACE_OLD cam=" << resolved_camera_id
              << " old=" << old << " new=" << p.session_id << std::endl;
    RemoveSession(old);
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_ || !initialized_) return;
  if (!signaling_ || !signaling_->IsAvailable()) return;
  if (sessions_.count(p.session_id) > 0 ||
      closing_sessions_.count(p.session_id) > 0) return;

  if (!old_sids.empty() || CameraHasPrivateBranchLocked(resolved_camera_id)) {
    pending_replacement_by_camera_[resolved_camera_id] = p;
    std::cerr << "[SessionManager] REPLACE_DEFER cam=" << resolved_camera_id
              << " new=" << p.session_id
              << " wait_teardown=1" << std::endl;
    return;
  }

  StartSessionLocked(p, resolved_camera_id);
}

void SessionManager::StartSessionLocked(const SessionNewPayload& p,
                                        const std::string& resolved_camera_id) {
  if (shutting_down_ || !initialized_) return;
  if (!signaling_ || !signaling_->IsAvailable()) return;
  if (sessions_.count(p.session_id) > 0 ||
      closing_sessions_.count(p.session_id) > 0) return;

  WebRtcSession::Options opts;
  opts.camera_id  = resolved_camera_id;
  opts.session_id = p.session_id;
  opts.session_generation = ++session_generation_;
  opts.encoder_generation = [this](const std::string& cam) { return EncoderGeneration(cam); };
  opts.ice_servers = p.ice_servers;
  opts.ice_gathering_timeout_ms = 10000;

  opts.on_offer = [this](const std::string& cam, const std::string& sid,
                         const std::string& sdp) {
    OnSessionOffer(cam, sid, sdp);
  };
  opts.on_ice_local = [this](const std::string& cam, const std::string& sid,
                             const std::string& mid, int idx,
                             const std::string& cand) {
    OnSessionIceLocal(cam, sid, mid, idx, cand);
  };
  opts.on_state_change = [this](const std::string& cam, const std::string& sid,
                                const std::string& ice, const std::string& peer) {
    OnSessionStateChange(cam, sid, ice, peer);
  };
  opts.ensure_tee = [this](const std::string& cam) -> GstElement* {
    return EnsureWebrtcEncoder(cam);
  };
  opts.allow_fku = [this](const std::string& cam) -> bool {
    return AllowForceKeyUnit(cam);
  };
  opts.register_rtp_probe = [this](const std::string& sid, GstElement* pay) {
    RegisterPeerRtpProbe(sid, pay);
  };
  opts.unregister_peer_health = [this](const std::string& sid) {
    UnregisterPeerHealth(sid);
  };
  opts.on_teardown_done = [this](const std::string& sid, bool timed_out) {
    OnPeerTeardownDone(sid, timed_out);
  };

  auto session = std::make_unique<WebRtcSession>(std::move(opts));
  sessions_[p.session_id] = std::move(session);
  session_born_us_[p.session_id] = g_get_monotonic_time();

  std::cout << "[SessionManager] New session: sid='" << p.session_id
            << "' cam='" << resolved_camera_id << "' ice_srv=" << p.ice_servers.size()
            << " payload_sid='" << p.session_id << "'" << std::endl;
  trace::Emit("SESSION_NEW_RX", resolved_camera_id, p.session_id, session_generation_,
              EncoderGeneration(resolved_camera_id), 0, "manager", "ok");

  QueueSessionInitLocked(resolved_camera_id, p.session_id);
}

void SessionManager::DrainPendingReplacementsLocked() {
  if (shutting_down_ || !initialized_) return;
  std::vector<std::pair<std::string, SessionNewPayload>> ready;
  for (auto it = pending_replacement_by_camera_.begin();
       it != pending_replacement_by_camera_.end();) {
    if (CameraHasPrivateBranchLocked(it->first)) {
      ++it;
      continue;
    }
    ready.emplace_back(it->first, std::move(it->second));
    it = pending_replacement_by_camera_.erase(it);
  }
  for (auto& item : ready) {
    std::cerr << "[SessionManager] REPLACE_START cam=" << item.first
              << " new=" << item.second.session_id << std::endl;
    StartSessionLocked(item.second, item.first);
  }
}

void SessionManager::OnAnswer(const SdpPayload& p) {
  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_) return;
  auto it = sessions_.find(p.session_id);
  if (it == sessions_.end()) {
    std::cerr << "[SessionManager] Answer for unknown session: "
              << p.session_id << std::endl;
    return;
  }
  trace::Emit("ANSWER_RX", it->second->GetCameraId(), p.session_id,
              it->second->GetSessionGeneration(),
              EncoderGeneration(it->second->GetCameraId()), 0,
              "manager", "ok", "answer_received");
  it->second->OnRemoteAnswer(p.sdp);
}

void SessionManager::OnIceRemote(const IceCandidatePayload& p) {
  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_) return;
  auto it = sessions_.find(p.session_id);
  if (it == sessions_.end()) return;
  trace::Emit("ICE_RX", it->second->GetCameraId(), p.session_id,
              it->second->GetSessionGeneration(),
              EncoderGeneration(it->second->GetCameraId()), 0,
              "manager", "ok", "candidate_received");
  it->second->OnRemoteIce(p.sdpMid, p.sdpMLineIndex, p.candidate);
}

void SessionManager::OnClose(const SessionClosePayload& p) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;
    for (auto it = pending_replacement_by_camera_.begin();
         it != pending_replacement_by_camera_.end();) {
      if (it->second.session_id == p.session_id) {
        std::cerr << "[SessionManager] pending replacement dropped cam="
                  << it->first << " sid=" << p.session_id << std::endl;
        it = pending_replacement_by_camera_.erase(it);
      } else {
        ++it;
      }
    }
  }
  std::cerr << "[SessionManager] Close request: " << p.session_id
            << " reason=" << p.reason << std::endl;
  trace::Emit("CLOSE_RX", "", p.session_id, 0, 0, 0,
              "manager", "ok", "close_received");
  RemoveSession(p.session_id);
}

void SessionManager::ApplySharedBranchCaps(const std::string& camera_id,
                                           int width, int height,
                                           int max_kbps) {
  if (!registry_) return;

  auto caps_it = registry_->webrtc_capsfilter_by_camera.find(camera_id);
  if (caps_it != registry_->webrtc_capsfilter_by_camera.end() && caps_it->second &&
      width > 0 && height > 0) {
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        nullptr);
    g_object_set(caps_it->second, "caps", caps, nullptr);
    gst_caps_unref(caps);
    std::cerr << "[SessionManager] caps " << camera_id
              << " → " << width << "x" << height << std::endl;
  }

  auto enc_it = registry_->webrtc_encoder_by_camera.find(camera_id);
  if (enc_it != registry_->webrtc_encoder_by_camera.end() && enc_it->second) {
    GstElement* enc = enc_it->second;
    if (max_kbps > 0) {
      GObjectClass* klass = G_OBJECT_GET_CLASS(enc);
      if (g_object_class_find_property(klass, "bitrate")) {
        g_object_set(enc, "bitrate", max_kbps * 1000, nullptr);
        std::cerr << "[SessionManager] enc bitrate " << camera_id
                  << " → " << (max_kbps * 1000) << " bps" << std::endl;
      } else {
        // Qualcomm v4l2h264enc often uses extra-controls
        GstStructure* ctrl = gst_structure_new(
            "v4l2_ctrl", "video_bitrate", G_TYPE_INT, max_kbps * 1000, nullptr);
        if (g_object_class_find_property(klass, "extra-controls")) {
          g_object_set(enc, "extra-controls", ctrl, nullptr);
          std::cerr << "[SessionManager] enc extra-controls bitrate " << camera_id
                    << " → " << (max_kbps * 1000) << std::endl;
        }
        gst_structure_free(ctrl);
      }
    } else {
      std::cerr << "[SessionManager] enc bitrate unlimited for " << camera_id
                << std::endl;
    }
  }
}

void SessionManager::OnRenegotiate(const RenegotiatePayload& p) {
  std::cerr << "[SessionManager] Renegotiate: " << p.session_id
            << " " << p.width << "x" << p.height
            << " max_kbps=" << p.max_kbps << std::endl;

  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(p.session_id);
  if (it == sessions_.end()) {
    std::cerr << "[SessionManager] Renegotiate for unknown session: "
              << p.session_id << std::endl;
    return;
  }

  ApplySharedBranchCaps(p.camera_id.empty() ? it->second->GetCameraId()
                                            : p.camera_id,
                        p.width, p.height, p.max_kbps);
  it->second->RequestRenegotiateOffer();
}

void SessionManager::OnSessionOffer(const std::string& camera_id,
                                    const std::string& session_id,
                                    const std::string& sdp) {
  std::cerr << "[SessionManager] OnSessionOffer: " << session_id
            << " sdp=" << sdp.size() << "b sig=" << (signaling_?"ok":"NULL") << std::endl;
  if (signaling_) {
    bool ok = signaling_->PublishOffer(camera_id, session_id, sdp);
    std::cerr << "[SessionManager] PublishOffer result: " << (ok?"OK":"FAIL") << std::endl;
  }
}

void SessionManager::OnSessionIceLocal(const std::string& camera_id,
                                       const std::string& session_id,
                                       const std::string& sdp_mid,
                                       int sdp_mline_index,
                                       const std::string& candidate) {
  if (signaling_) {
    signaling_->PublishIceLocal(camera_id, session_id, sdp_mid,
                                sdp_mline_index, candidate);
  }
}

void SessionManager::OnSessionStateChange(const std::string& camera_id,
                                          const std::string& session_id,
                                          const std::string& ice_state,
                                          const std::string& peer_state) {
  if (signaling_) {
    signaling_->PublishState(camera_id, session_id, ice_state, peer_state);
  }

  {
    // Must NOT take mu_: may be called from DoClose under ProcessAllQueues.
    std::lock_guard<std::mutex> lock(ice_state_mu_);
    last_ice_state_[session_id] = ice_state;
  }

  // Server-side reclaim when the peer goes away without an explicit close
  // (tab switch that outlives the client grace, browser kill, network loss).
  // Reclaim MUST be async: we are inside a webrtcbin signal callback, so
  // destroying this WebRtcSession synchronously would tear down the element
  // that is calling us. Defer to the GLib main loop.
  // Do NOT treat ICE "closed" as instant reclaim: webrtcbin can notify closed
  // on create / our own DoClose. Browser-gone is ice_failed, disconnected>8s,
  // or rtp_stale/rtp_never. Never key off a newer sid on the same camera.
  if (ice_state == "failed") {
    g_idle_add(&SessionManager::ReclaimNowCb,
               new ReclaimCtx{this, session_id, "ice_failed"});
  } else if (ice_state == "disconnected") {
    g_timeout_add_seconds(8, &SessionManager::ReclaimIfDisconnectedCb,
                          new ReclaimCtx{this, session_id, "ice_disconnected"});
  }
}

gboolean SessionManager::ReclaimNowCb(gpointer user_data) {
  auto* ctx = static_cast<ReclaimCtx*>(user_data);
  const char* why = ctx->why ? ctx->why : "ice_failed";
  bool still_here = false;
  {
    std::lock_guard<std::mutex> lock(ctx->self->mu_);
    still_here = ctx->self->sessions_.count(ctx->session_id) > 0;
  }
  if (still_here) {
    std::cerr << "[SessionManager] RECLAIM sid=" << ctx->session_id
              << " why=" << why << std::endl;
    ctx->self->RemoveSession(ctx->session_id);
  }
  delete ctx;
  return G_SOURCE_REMOVE;
}

gboolean SessionManager::ReclaimIfDisconnectedCb(gpointer user_data) {
  auto* ctx = static_cast<ReclaimCtx*>(user_data);
  bool still_gone = false;
  {
    std::lock_guard<std::mutex> lock(ctx->self->ice_state_mu_);
    auto it = ctx->self->last_ice_state_.find(ctx->session_id);
    still_gone = (it != ctx->self->last_ice_state_.end() &&
                  it->second == "disconnected");
  }
  if (still_gone) {
    std::cerr << "[SessionManager] RECLAIM sid=" << ctx->session_id
              << " why=" << (ctx->why ? ctx->why : "ice_disconnected")
              << std::endl;
    ctx->self->RemoveSession(ctx->session_id);
  }
  delete ctx;
  return G_SOURCE_REMOVE;
}

void SessionManager::RemoveSession(const std::string& session_id) {
  WebRtcSession* closer = nullptr;
  std::unique_ptr<WebRtcSession> immediate;
  std::string camera_id;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closing_sessions_.count(session_id) > 0) return;
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    camera_id = it->second->GetCameraId();
    auto pending = pending_init_by_camera_.find(camera_id);
    if (pending != pending_init_by_camera_.end()) {
      auto& ids = pending->second;
      ids.erase(std::remove(ids.begin(), ids.end(), session_id), ids.end());
      if (ids.empty()) pending_init_by_camera_.erase(pending);
    }
    session_born_us_.erase(session_id);
    session_armed_us_.erase(session_id);

    if (it->second->TeardownFinished()) {
      immediate = std::move(it->second);
      sessions_.erase(it);
    } else {
      closing_sessions_[session_id] = std::move(it->second);
      sessions_.erase(it);
      closer = closing_sessions_[session_id].get();
    }
  }
  {
    std::lock_guard<std::mutex> lock(ice_state_mu_);
    last_ice_state_.erase(session_id);
  }
  if (registry_) {
    UnregisterPeerHealth(session_id);
  }
  if (closer) {
    // Enqueue Close only. Do not ProcessQueue / destroy here (NATS thread UAF).
    closer->Close();
  }
  immediate.reset();
  MaybeTeardownEncoder(camera_id);
}

void SessionManager::OnPeerTeardownDone(const std::string& session_id,
                                        bool timed_out) {
  std::unique_ptr<WebRtcSession> doomed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = closing_sessions_.find(session_id);
    if (it != closing_sessions_.end()) {
      doomed = std::move(it->second);
      closing_sessions_.erase(it);
    }
    if (timed_out && !shutting_down_ && !circuit_breaker_tripped_) {
      circuit_breaker_tripped_ = true;
      circuit_breaker_pending_notify_ = true;
      std::cerr << "[ERROR] Gate3 peer teardown timeout sid=" << session_id
                << " — circuit-breaker open — exiting for runner retry"
                << std::endl;
    }
  }
  if (!doomed) return;
  // Destroy on a later idle so FinishAsyncTeardown can return off *this.
  struct DestroyCtx {
    std::unique_ptr<WebRtcSession> session;
  };
  auto* ctx = new DestroyCtx{std::move(doomed)};
  g_idle_add(
      [](gpointer user_data) -> gboolean {
        delete static_cast<DestroyCtx*>(user_data);
        return G_SOURCE_REMOVE;
      },
      ctx);
}

void SessionManager::MaybeTeardownEncoder(const std::string& camera_id) {
  // CP05-R1A: shared encoder is pipeline-owned. Last peer close must not
  // destroy Venus/V4L2 encoder hardware; whole-pipeline NULL releases it.
  std::lock_guard<std::mutex> lock(mu_);
  auto state = encoder_state_by_camera_.find(camera_id);
  if (state == encoder_state_by_camera_.end() ||
      state->second != EncoderState::kReady || HasLiveSessionLocked(camera_id)) {
    return;
  }
  std::cerr << "[CP05-R1A] skip encoder teardown on last session close cam="
            << camera_id << " gen=" << EncoderGeneration(camera_id)
            << " (pipeline-owned until whole-pipeline NULL)" << std::endl;
}

}  // namespace aibox::webrtc
