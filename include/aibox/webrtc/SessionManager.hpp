#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gst/gst.h>
#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/runtime/RuntimeRegistry.hpp"
#include "aibox/webrtc/WebRtcSession.hpp"
#include "aibox/webrtc/WebrtcSignaling.hpp"

namespace aibox::webrtc {

class SessionManager {
 public:
  SessionManager();
  ~SessionManager();

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  bool Init(GstElement* pipeline, adapter::NatsAdapter* nats,
            pipeline::RuntimeRegistry* registry = nullptr);

  void RegisterCamera(const std::string& camera_id, GstElement* tee_webrtc);

  /// P3b: if tee missing, convert splitter stub → enc→tee (must run on main/GLib thread).
  GstElement* EnsureWebrtcEncoder(const std::string& camera_id);

  /// Observe encoder stalls. Never live-rebuild.
  /// Gate 3: empty live-READY never trips; a single stalled cam only WARNs;
  /// trip only if all live-READY are stalled, or >= half have been stalled
  /// continuously for 30s. Planned Close / REPLACE / encoder-build and a 5s
  /// settle after live-set change never trip. Dead peers (rtp_stale /
  /// rtp_never) are collected after the serial first-frame wave.
  /// Call from main loop (~200ms).
  void TickWatchdog();

  /// True once after stall budget trips; clears the latch for the caller.
  /// Main loop should quit and let the runner restart the whole pipeline.
  bool ConsumeCircuitBreakerTrip();

  /// CP05-R1B: true while an encoder build awaits first-frame (or timeout).
  /// Watchdog still skips stall accounting during a serial build wave.
  /// DoInit is no longer blocked on this flag (per-cam offer); ProcessAllQueues
  /// rate-limits DoInit to 1 per tick instead.
  bool IsEncoderBuildInProgress() const;

  /// Harden: ≤1 force-key-unit / second / camera. Safe without holding mu_.
  bool AllowForceKeyUnit(const std::string& camera_id);

  bool StartSignaling();
  void Shutdown();
  int ProcessAllQueues();
  bool HasCamera(const std::string& camera_id) const;

 private:
  enum class EncoderState { kAbsent, kQueued, kBuilding, kReady, kFailed, kTearingDown };

  // parse.src BUFFER probe context (owned via shared_ptr in registry map helper)
  struct EncProbeCtx {
    SessionManager* self = nullptr;
    std::string camera_id;
    std::shared_ptr<std::atomic<gint64>> last_us;
    std::shared_ptr<std::atomic<guint64>> parsed_count;
    std::atomic<bool> first_seen{false};
    int generation = 0;
  };

  struct HealthProbeCtx {
    std::shared_ptr<std::atomic<guint64>> count;
    std::shared_ptr<std::atomic<gint64>> last_us;
  };

  struct PadProbeHandle {
    GstPad* pad = nullptr;  // owned ref
    gulong id = 0;
    std::shared_ptr<void> keep_alive;
  };

  // Destroy-notify holders for pad-probe user_data: the shared_ptr keeps the
  // probe context alive until the probe is REMOVED (gst_pad_remove_probe
  // waits for in-flight callbacks; the destroy notify runs afterwards).
  // Passing ctx.get() without a holder is a use-after-free hazard.
  struct EncProbeHolder { std::shared_ptr<EncProbeCtx> ctx; };
  struct HealthProbeHolder { std::shared_ptr<HealthProbeCtx> ctx; };
  static void EncProbeHolderFree(gpointer data);
  static void HealthProbeHolderFree(gpointer data);

  struct FirstFrameNotify {
    SessionManager* self = nullptr;
    std::string camera_id;
    int generation = 0;
  };

  struct FirstFrameTimeout {
    SessionManager* self = nullptr;
    std::string camera_id;
    int generation = 0;
  };

  /// One live peer per camera. If a private branch is still ACTIVE /
  /// INITIALIZING / TEARING_DOWN, stash the latest session.new as a pending
  /// replacement and create it only after SESSION_CLOSE_DONE.
  void OnNewSession(const SessionNewPayload& p);
  void StartSessionLocked(const SessionNewPayload& p,
                          const std::string& camera_id);
  void DrainPendingReplacementsLocked();
  bool CameraHasPrivateBranchLocked(const std::string& camera_id) const;
  void OnAnswer(const SdpPayload& p);
  void OnIceRemote(const IceCandidatePayload& p);
  void OnClose(const SessionClosePayload& p);
  void OnRenegotiate(const RenegotiatePayload& p);

  void OnSessionOffer(const std::string& camera_id, const std::string& session_id,
                      const std::string& sdp);
  void OnSessionIceLocal(const std::string& camera_id, const std::string& session_id,
                         const std::string& sdp_mid, int sdp_mline_index,
                         const std::string& candidate);
  void OnSessionStateChange(const std::string& camera_id, const std::string& session_id,
                            const std::string& ice_state, const std::string& peer_state);

  void RemoveSession(const std::string& session_id);
  void OnPeerTeardownDone(const std::string& session_id, bool timed_out);
  void ApplySharedBranchCaps(const std::string& camera_id, int width, int height,
                             int max_kbps);

  // Build enc GstBin off tee_perm; install out-frame probe. Caller must hold
  // main-thread context; does NOT lock mu_.
  // camera_id passed by value: callers may hold a reference into a container
  // (e.g. registry map key) that these methods mutate/erase; copying avoids
  // dangling-reference UB.
  GstElement* BuildEncoderChain(std::string camera_id);
  // Retained for diagnostics / future controlled experiments; CP05-R1A
  // TickWatchdog must not call this on the live path.
  bool RebuildWebrtcEncoder(std::string camera_id);
  void TeardownEncoderChain(std::string camera_id);
  int EncoderGeneration(const std::string& camera_id) const;
  void QueueSessionInitLocked(const std::string& camera_id,
                              const std::string& session_id);
  // Next structure waits for this camera's first encoded frame (Venus).
  // Offer/Init already ran at structure-up.
  void ScheduleEncoderBuildLocked();
  void RunEncoderBuild();
  void CompleteEncoderReady(const std::string& camera_id, int generation);
  void FailEncoderBuild(const std::string& camera_id, const char* reason);
  void CancelFirstFrameTimeoutLocked(const std::string& camera_id);
  void ArmFirstFrameTimeoutLocked(const std::string& camera_id, int generation,
                                  guint seconds);
  // Soft-extend first-frame wait once (no teardown) for cold Venus hiccups.
  bool SoftExtendFirstFrameOnce(const std::string& camera_id, int generation);
  // Cold-start retry: teardown + requeue, keep pending sessions (budget 2).
  bool RetryEncoderBuildOnce(const std::string& camera_id, const char* reason);
  static gboolean EncoderBuildCb(gpointer user_data);
  static gboolean FirstFrameIdleCb(gpointer user_data);
  static gboolean FirstFrameTimeoutCb(gpointer user_data);
  static gboolean DeferredSessionInitCb(gpointer user_data);

  struct DeferredInitCtx {
    SessionManager* self = nullptr;
    std::vector<std::string> session_ids;
    std::string camera_id;
  };
  bool HasLiveSessionLocked(const std::string& camera_id) const;
  // CP05-R1A: last-peer close must NOT destroy shared encoder/Venus.
  // Encoder remains pipeline-owned until whole-pipeline NULL.
  void MaybeTeardownEncoder(const std::string& camera_id);

  // ICE / RTP reclaim: only this sid is dead. Never "newer sid on same cam
  // → kill old" (that would block two-page later; REPLACE_OLD stays Gate 1).
  struct ReclaimCtx {
    SessionManager* self = nullptr;
    std::string     session_id;
    const char*     why = nullptr;  // ice_failed | ice_disconnected | rtp_*
  };
  static gboolean ReclaimNowCb(gpointer user_data);            // ICE failed
  static gboolean ReclaimIfDisconnectedCb(gpointer user_data); // ICE disconnected grace
  // Caller holds mu_. Appends {sid, why} for rtp_stale / rtp_never.
  void CollectDeadPeersLocked(
      gint64 now, std::vector<std::pair<std::string, const char*>>& out);

  static GstPadProbeReturn EncOutProbe(GstPad* pad, GstPadProbeInfo* info,
                                       gpointer user_data);
  static GstPadProbeReturn HealthBufProbe(GstPad* pad, GstPadProbeInfo* info,
                                          gpointer user_data);

  void AttachHealthProbe(std::string camera_id, GstPad* pad,
                         std::shared_ptr<std::atomic<guint64>> count,
                         std::shared_ptr<std::atomic<gint64>> last_us);
  void ClearHealthProbes(const std::string& camera_id);
  void RegisterPeerRtpProbe(const std::string& session_id, GstElement* pay);
  void UnregisterPeerHealth(const std::string& session_id);
  void TickHealthPrint();

  GstElement*  pipeline_ = nullptr;
  adapter::NatsAdapter* nats_ = nullptr;
  pipeline::RuntimeRegistry* registry_ = nullptr;

  mutable std::mutex mu_;
  bool initialized_ = false;
  bool shutting_down_ = false;

  std::unique_ptr<WebrtcSignaling> signaling_;
  std::unordered_map<std::string, std::unique_ptr<WebRtcSession>> sessions_;
  // Gate 3: unique_ptr lives here until async IDLE unlink + private NULL finish.
  std::unordered_map<std::string, std::unique_ptr<WebRtcSession>> closing_sessions_;
  // Latest session.new waiting for the previous private branch to finish teardown.
  std::unordered_map<std::string, SessionNewPayload> pending_replacement_by_camera_;
  std::unordered_map<std::string, gint64> session_born_us_;   // sid -> OnNewSession
  std::unordered_map<std::string, gint64> session_armed_us_;  // first !kPending tick
  std::unordered_map<std::string, GstElement*> webrtc_tees_;
  // Separate from mu_: PublishState/DoClose may run while ProcessAllQueues
  // already holds mu_ (non-recursive). Touching last_ice_state_ under mu_
  // deadlocks the GLib main loop and makes SIGTERM appear ignored.
  mutable std::mutex ice_state_mu_;
  std::unordered_map<std::string, std::string> last_ice_state_;  // sid -> last ice state
  std::unordered_map<std::string, int> encoder_generation_by_camera_;
  std::unordered_map<std::string, EncoderState> encoder_state_by_camera_;
  std::unordered_map<std::string, std::vector<std::string>> pending_init_by_camera_;
  std::deque<std::string> encoder_build_queue_;
  guint encoder_build_source_ = 0;
  // True from BuildEncoderChain until first-frame (or fail/retry).
  bool encoder_build_in_progress_ = false;
  std::string encoder_build_camera_;
  int encoder_build_generation_ = 0;
  std::unordered_map<std::string, guint> first_frame_timeout_by_camera_;
  // Per-camera soft-extend budget (0 unused, 1 already extended once).
  std::unordered_map<std::string, int> first_frame_soft_extend_by_camera_;
  // Per-camera first-frame teardown/requeue budget (0 unused … 2 = exhausted).
  std::unordered_map<std::string, int> first_frame_retry_by_camera_;
  // Keep probe ctx alive for first-frame CAS + last_us updates.
  std::unordered_map<std::string, std::shared_ptr<EncProbeCtx>> enc_probe_ctx_by_camera_;
  std::unordered_map<std::string, std::vector<PadProbeHandle>> health_probes_by_camera_;
  std::unordered_map<std::string, PadProbeHandle> peer_rtp_probes_;
  gint64 health_last_print_us_ = 0;
  mutable std::mutex generation_mu_;
  int session_generation_ = 0;

  // Stall observe + FKU throttle (separate from mu_ to avoid deadlock)
  std::mutex selfheal_mu_;
  std::unordered_map<std::string, gint64> selfheal_last_us_;  // cam -> last stall warn
  std::unordered_map<std::string, gint64> fku_last_us_;       // cam -> last FKU
  gint64 watchdog_last_tick_us_ = 0;
  int enc_rebuild_gen_ = 0;  // uniquify element names across rebuilds
  // Gate 3: continuous stall start (cam -> first tick it was 5s stale).
  std::unordered_map<std::string, gint64> stall_since_us_;
  std::set<std::string> last_live_set_;
  gint64 topology_settle_until_us_ = 0;
  bool circuit_breaker_tripped_ = false;
  bool circuit_breaker_pending_notify_ = false;
};

}  // namespace aibox::webrtc
