#include "aibox/pipeline/GstPipelineBuilder.hpp"
#include "aibox/pipeline/GstElementHelper.hpp"
#include "aibox/pipeline/GstLinkHelper.hpp"
#include "aibox/pipeline/BatchLockstep.hpp"
#include "../../plugins/aiboxframemeta_common/gstaiboxframemeta.h"
#include "aibox/face/face_detection_meta_types.hpp"
#include "aibox/face/face_result_state_meta_types.hpp"
#include "aibox/face/face_mask_policy.hpp"
#include "aibox/pipeline/FaceOcOverlay.hpp"

#include <gst/video/video.h>
#include <gst/gsttask.h>  // GstTaskState / gst_pad_get_task_state (diagq)
#ifdef HAVE_DETECTION_NATS
#include "aibox/detection/CaptureTimestampTracker.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gst/app/gstappsink.h>

namespace aibox::pipeline {

// ===================================================================
// RequestPadHandle — move-only RAII, strong owner ref
// ===================================================================
RequestPadHandle::RequestPadHandle(GstElement* owner, GstPad* pad)
    : owner_(owner ? GST_ELEMENT(gst_object_ref(owner)) : nullptr),
      pad_(pad ? GST_PAD(gst_object_ref(pad)) : nullptr) {}

RequestPadHandle::~RequestPadHandle() { Reset(); }

RequestPadHandle::RequestPadHandle(RequestPadHandle&& other) noexcept
    : owner_(other.owner_), pad_(other.pad_) {
  other.owner_ = nullptr;
  other.pad_   = nullptr;
}

RequestPadHandle& RequestPadHandle::operator=(RequestPadHandle&& other) noexcept {
  if (this != &other) {
    Reset();
    owner_ = other.owner_;
    pad_   = other.pad_;
    other.owner_ = nullptr;
    other.pad_   = nullptr;
  }
  return *this;
}

void RequestPadHandle::Reset() noexcept {
  if (pad_ && owner_) {
    GstPad* peer = gst_pad_get_peer(pad_);
    if (peer) {
      if (GST_PAD_IS_SRC(pad_))
        gst_pad_unlink(pad_, peer);
      else
        gst_pad_unlink(peer, pad_);
      gst_object_unref(peer);
    }
    gst_element_release_request_pad(owner_, pad_);
  }
  if (pad_)   { gst_object_unref(pad_);   pad_   = nullptr; }
  if (owner_) { gst_object_unref(owner_); owner_ = nullptr; }
}

void ReleaseAllRequestPads(RuntimeRegistry& reg) {
  for (auto& h : reg.request_pad_handles) h.Reset();
  reg.request_pad_handles.clear();
}

void DetachBatchQnnProbes(RuntimeRegistry& /*reg*/) {}

void PrepareBatchTeardown(RuntimeRegistry& reg) {
  for (auto& kv : reg.batch_group_runtime) {
    BatchGroupRuntime& br = kv.second;
    if (br.lockstep)
      batch_lockstep_shutdown(br.lockstep.get());
    for (int i = 0; i < 2; ++i) {
      if (br.lockstep_pad[i] && br.lockstep_probe_id[i] != 0) {
        gst_pad_remove_probe(br.lockstep_pad[i], br.lockstep_probe_id[i]);
        br.lockstep_probe_id[i] = 0;
      }
      if (br.lockstep_pad[i]) {
        gst_object_unref(br.lockstep_pad[i]);
        br.lockstep_pad[i] = nullptr;
      }
    }
  }
}

static bool ShouldDispatchBatch(const ExecutionPlan& plan) {
  return plan.execution_mode == ExecutionMode::kMultistreamBatch &&
         !plan.batch_group_plans.empty();
}

// ===================================================================
// rtspsrc pad-added context & callback
// ===================================================================
struct RtspPadCtx {
  std::string camera_id;
  GstElement* depay = nullptr;
};

static bool IsV4l2HwDecoder(const std::string& name) {
  return name == "v4l2h264dec" || name == "v4l2h265dec";
}

static bool IsRtspVideoCodec(const gchar* encoding) {
  if (!encoding) return false;
  return g_ascii_strcasecmp(encoding, "H264") == 0 ||
         g_ascii_strcasecmp(encoding, "H265") == 0 ||
         g_ascii_strcasecmp(encoding, "HEVC") == 0;
}

static void OnRtspPadAdded(GstElement* src, GstPad* new_pad, gpointer data) {
  (void)src;
  auto* ctx = static_cast<RtspPadCtx*>(data);
  GstPad* sink = gst_element_get_static_pad(ctx->depay, "sink");
  if (!sink) return;
  if (gst_pad_is_linked(sink)) { gst_object_unref(sink); return; }

  // Prefer current caps; if empty (common before first packet), query caps.
  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
  if (!caps || gst_caps_is_empty(caps)) {
    fprintf(stderr, "[RTSP] cam=%s pad=%s: no caps yet, skip link\n",
            ctx->camera_id.c_str(), GST_PAD_NAME(new_pad));
    if (caps) gst_caps_unref(caps);
    gst_object_unref(sink);
    return;
  }

  GstStructure* s = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(s);
  const gchar* media = gst_structure_get_string(s, "media");
  const gchar* encoding = gst_structure_get_string(s, "encoding-name");
  const bool is_rtp = (name && g_str_has_prefix(name, "application/x-rtp"));
  const bool is_video = (!media || g_ascii_strcasecmp(media, "video") == 0);

  if (!(is_rtp && is_video && IsRtspVideoCodec(encoding))) {
    fprintf(stderr,
            "[RTSP] cam=%s pad=%s: skip non-video RTP "
            "(name=%s media=%s encoding=%s)\n",
            ctx->camera_id.c_str(), GST_PAD_NAME(new_pad),
            name ? name : "(null)", media ? media : "(null)",
            encoding ? encoding : "(null)");
    gst_caps_unref(caps);
    gst_object_unref(sink);
    return;
  }

  GstPadLinkReturn lr = gst_pad_link(new_pad, sink);
  if (lr != GST_PAD_LINK_OK) {
    fprintf(stderr, "[RTSP] cam=%s pad=%s: link to depay FAILED (%d) encoding=%s\n",
            ctx->camera_id.c_str(), GST_PAD_NAME(new_pad), (int)lr,
            encoding ? encoding : "(null)");
  } else {
    fprintf(stderr, "[RTSP] cam=%s pad=%s: linked %s video RTP -> depay\n",
            ctx->camera_id.c_str(), GST_PAD_NAME(new_pad),
            encoding ? encoding : "?");
  }
  gst_caps_unref(caps);
  gst_object_unref(sink);
}

/* Dynamically create GstAiboxFrameMeta and fill camera_id + frame_id.
 * Other fields left empty/zero; aiboxtimemux completes them. */
static GstAiboxFrameMeta *
add_aibox_frame_meta_dynamic(GstBuffer *buf, const gchar *camera_id, guint64 frame_id)
{
  static const GstMetaInfo *mi = nullptr;
  if (G_UNLIKELY(!mi)) {
    mi = gst_meta_get_info("GstAiboxFrameMeta");
    if (!mi) {
      g_critical("GstAiboxFrameMeta NOT registered — "
                 "trigger_aibox_frame_meta_registration() failed or was not called");
      return nullptr;
    }
  }
  auto *m = (GstAiboxFrameMeta *)gst_buffer_add_meta(buf, mi, nullptr);
  if (!m) return nullptr;

  m->camera_id          = g_strdup(camera_id);
  m->frame_id           = frame_id;
  m->algorithm_group_id = g_strdup("");
  m->time_mux_group_id  = g_strdup("");
  m->mux_slot_id        = 0;
  m->pts                = GST_BUFFER_PTS(buf);
  m->global_cam_index   = 0;
  m->batch_group_id     = nullptr;
  m->batch_stream_id    = GST_AIBOX_FRAME_META_BATCH_INVALID_STREAM_ID;
  return m;
}

struct FrameTagCtx {
  guint64 frame_counter = 0;
  gchar  *camera_id = nullptr;
  gchar  *batch_group_id = nullptr;
  guint   batch_stream_id = GST_AIBOX_FRAME_META_BATCH_INVALID_STREAM_ID;
};

static void
frame_tag_ctx_free(gpointer data)
{
  auto *ctx = static_cast<FrameTagCtx *>(data);
  g_free(ctx->camera_id);
  g_free(ctx->batch_group_id);
  g_free(ctx);
}

/* Bound queue by buffer count only (disable default bytes/time caps).
 * leaky: 0=none (default), 2=downstream (drop oldest when full). */
static void ConfigureBoundedQueue(GstElement* q, guint max_buffers,
                                  gint leaky = 0) {
  if (!q) return;
  g_object_set(q,
               "max-size-buffers", max_buffers,
               "max-size-bytes", (guint)0,
               "max-size-time", (guint64)0,
               "leaky", leaky,
               nullptr);
}

// Sample (camera_id, video_frame_id, PTS) + result state (detection_frame_id,
// age_ms) at qtifaceinfer.src / qtivoverlay.sink. det_frame != vframe is
// EXPECTED in best-effort mode (results lag the video by 1-2 frames).
static void LogFaceSyncTriple(const char* where, GstBuffer* buf) {
  static GType fm_api = G_TYPE_INVALID;
  static GType fs_api = G_TYPE_INVALID;
  if (fm_api == G_TYPE_INVALID)
    fm_api = g_type_from_name("GstAiboxFrameMetaAPI");
  if (fs_api == G_TYPE_INVALID)
    fs_api = g_type_from_name("GstFaceResultStateMetaAPI");
  GstAiboxFrameMeta* fm =
      (fm_api != G_TYPE_INVALID)
          ? reinterpret_cast<GstAiboxFrameMeta*>(gst_buffer_get_meta(buf, fm_api))
          : nullptr;
  // Same video frame_id on both pads so the two logs can be compared directly.
  if (!fm || (fm->frame_id % 90) != 0) return;

  guint64 det_frame = 0;
  guint32 age_ms = 0;
  gboolean have_state = FALSE;
  if (fs_api != G_TYPE_INVALID) {
    const GstFaceResultStateMeta* sm =
        reinterpret_cast<const GstFaceResultStateMeta*>(
            gst_buffer_get_meta(buf, fs_api));
    if (sm && sm->abi_version == FACE_RESULT_STATE_META_ABI_VERSION) {
      det_frame = sm->detection_frame_id;
      age_ms = sm->result_age_ms;
      have_state = TRUE;
    }
  }
  fprintf(stderr,
          "[face-sync] %s cam=%s vframe=%llu det_frame=%llu age_ms=%u "
          "have_state=%d pts=%" GST_TIME_FORMAT "\n",
          where,
          fm->camera_id ? fm->camera_id : "(no-cam)",
          static_cast<unsigned long long>(fm->frame_id),
          static_cast<unsigned long long>(det_frame),
          age_ms, have_state ? 1 : 0,
          GST_TIME_ARGS(GST_BUFFER_PTS(buf)));
}

static GstPadProbeReturn
OnFaceSyncSampleProbe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buf)
    LogFaceSyncTriple(static_cast<const char*>(user_data), buf);
  return GST_PAD_PROBE_OK;
}

// Probe ctx: overlay element + cached caps dims (box clamping) + mask
// dedup cache (g_object_set only when the mask content actually changes).
struct FaceOverlayProbeCtx {
  GstElement* overlay = nullptr;
  int diag_group = 0;  // group ordinal FIXED at attach time. Expert rule: probe
                       // runtime code must NEVER read the mutable g_diag_group
                       // global (g1.ov.sink previously counted g0+g1 frames).
};

static void
FaceOverlayProbeCtxFree(gpointer data) {
  auto* ctx = static_cast<FaceOverlayProbeCtx*>(data);
  if (!ctx) return;
  /* FaceOverlayProbeCtx contains std::string — it MUST be allocated with
   * new and released with delete so its constructor/destructor run.
   * g_new0/g_free would skip both (definite UB, board SIGSEGV). */
  delete ctx;
}

// ---------------------------------------------------------------------------
// 1Hz aggregate diag points (expert P0-3): cumulative count + last-buffer-age
// per point, printed once per second. NO per-frame logging. Enabled via
// AIBOX_FACE_DIAG_1HZ=1. AIBOX_FACE_MASKS_OFF=1 skips masks g_object_set
// (B scenario) while keeping the overlay and the counters.
// ---------------------------------------------------------------------------
namespace {
enum DiagPointId : int {
  D_LM_SRC = 0,   // last_metamux.src
  D_QFI_SINK,     // q_face_in.sink
  D_QFI_SRC,      // q_face_in.src
  D_FI_SINK,      // qtifaceinfer.sink
  D_FI_SRC,       // qtifaceinfer.src
  D_QFO_SINK,     // q_face_out.sink
  D_QFO_SRC,      // q_face_out.src
  D_OV_SINK,      // overlay.sink (probe-internal)
  D_OV_SRC,       // overlay.src
  D_TEE_POST,     // tee_post.sink
  D_QDM_SINK,     // q_demux.sink
  D_QDM_SRC,      // q_demux.src
  D_DM_SRC_BASE = 12,                  // demux src_%d (+ slot index, up to 16)
  D_ENC_SINK_BASE = D_DM_SRC_BASE + 16,  // per-cam encoder sink (slots 0..15)
  D_ENC_SRC_BASE = D_ENC_SINK_BASE + 16, // per-cam encoder src (slots 0..15)
  kDiagPointsPerGroup = D_ENC_SRC_BASE + 16,
  // per-group id offset so group counters never mix (64 per group).
  // Expert rule: the group stride MUST cover one group's full point range,
  // otherwise g1's points overlap g0's and counters silently accumulate.
  kDiagGroupOffset = 64,
  kDiagPointCount = kDiagPointsPerGroup + kDiagGroupOffset * 3,
};
// BARE enumerators auto-increment (D_ENC_SINK_BASE was once 13 instead of
// 28 because the "= base+16" was only in the comment) — pin the layout at
// compile time so a future edit can't silently re-collide the ranges.
static_assert(D_ENC_SINK_BASE == D_DM_SRC_BASE + 16,
              "diag enc.sink range must follow dm.src range");
static_assert(D_ENC_SRC_BASE == D_ENC_SINK_BASE + 16,
              "diag enc.src range must follow enc.sink range");
static_assert(kDiagGroupOffset >= kDiagPointsPerGroup,
              "diag group stride must cover all per-group points");

struct DiagPoint {
  std::atomic<guint64> count{0};
  std::atomic<gint64> last_us{0};
};

DiagPoint g_diag[kDiagPointCount];
const char* g_diag_name[kDiagPointCount];
bool g_diag_enabled = false;
guint g_diag_timer = 0;
int g_diag_group = 0;  // current group ordinal (per-group id isolation)

/* q_face_in / q_face_out elements per group for the [diagq] low-perturbation
 * state sampling (expert round 3: task state / current-level / pad flags).
 * qfi is NULL when AIBOX_FACE_SKIP_QFACE_IN=1 (B mode has no pre-infer
 * queue); q_face_out exists in both modes (B2 froze at its src boundary). */
GstElement* g_diag_qfi[3] = {nullptr, nullptr, nullptr};
GstElement* g_diag_qfo[3] = {nullptr, nullptr, nullptr};

/* masks g_object_set latency ring (probe thread writes, timer reads) */
std::mutex g_setring_mtx;
std::array<gint64, 256> g_setring{};
int g_setring_pos = 0;
int g_setring_n = 0;

GstPadProbeReturn DiagBufProbe(GstPad*, GstPadProbeInfo*, gpointer user_data) {
  const int id = GPOINTER_TO_INT(user_data);
  if (id < 0 || id >= kDiagPointCount) return GST_PAD_PROBE_OK;
  g_diag[id].count.fetch_add(1, std::memory_order_relaxed);
  g_diag[id].last_us.store(g_get_monotonic_time(), std::memory_order_relaxed);
  return GST_PAD_PROBE_OK;
}

gboolean DiagTimerCb(gpointer) {
  const gint64 now = g_get_monotonic_time();
  std::string line = "[diag1hz]";
  char b[96];
  for (int i = 0; i < kDiagPointCount; ++i) {
    const guint64 c = g_diag[i].count.load(std::memory_order_relaxed);
    const gint64 lu = g_diag[i].last_us.load(std::memory_order_relaxed);
    if (c == 0 && lu == 0) continue;
    const gint64 age_ms = lu ? (now - lu) / 1000 : -1;
    const int group = i / kDiagGroupOffset;
    const int base = i % kDiagGroupOffset;
    snprintf(b, sizeof(b), " g%u.%s=%llu/%lldms", group,
             g_diag_name[base] ? g_diag_name[base] : "?",
             static_cast<unsigned long long>(c), static_cast<long long>(age_ms));
    line += b;
  }
  {
    std::lock_guard<std::mutex> lk(g_setring_mtx);
    if (g_setring_n > 0) {
      std::array<gint64, 256> s;
      std::copy(g_setring.begin(), g_setring.begin() + g_setring_n, s.begin());
      std::sort(s.begin(), s.begin() + g_setring_n);
      auto pct = [&](double p) {
        return s[static_cast<size_t>(p / 100.0 * (g_setring_n - 1))];
      };
      snprintf(b, sizeof(b),
               " ov.set_p50/95/99/max=%lld/%lld/%lld/%lldus set_n=%d",
               static_cast<long long>(pct(50)), static_cast<long long>(pct(95)),
               static_cast<long long>(pct(99)),
               static_cast<long long>(s[g_setring_n - 1]), g_setring_n);
      line += b;
    }
  }
  fprintf(stderr, "%s\n", line.c_str());

  /* [diagq] low-perturbation queue state per group (expert round 3):
   * gst_pad_get_task_state + current-level-* + pad active/linked/flushing.
   * 1Hz only, public APIs, no probes — must not perturb the streaming path.
   * Covers BOTH face queues: pre-infer (qfi, A mode only) and post-infer
   * (qfo, both modes — B2 froze at its src boundary). */
  for (int g = 0; g < 3; ++g) {
    GstElement* qs[2] = {g_diag_qfi[g], g_diag_qfo[g]};
    const int sk_ids[2] = {D_QFI_SINK, D_QFO_SINK};
    const int sr_ids[2] = {D_QFI_SRC, D_QFO_SRC};
    for (int k = 0; k < 2; ++k) {
      GstElement* q = qs[k];
      if (!q) continue;
      guint lvl_b = 0;
      guint64 lvl_bytes = 0, lvl_time = 0;
      g_object_get(q, "current-level-buffers", &lvl_b,
                      "current-level-bytes", &lvl_bytes,
                      "current-level-time", &lvl_time, nullptr);
      GstPad* sp = gst_element_get_static_pad(q, "src");
      GstPad* sk = gst_element_get_static_pad(q, "sink");
      const char* ts = "?";
      if (sp) {
        const GstTaskState st = gst_pad_get_task_state(sp);
        ts = (st == GST_TASK_STARTED) ? "STARTED"
             : (st == GST_TASK_PAUSED)  ? "PAUSED"
             : (st == GST_TASK_STOPPED) ? "STOPPED" : "?";
      }
      const int qi = g * kDiagGroupOffset;
      const guint64 qsink_c = g_diag[qi + sk_ids[k]].count.load(std::memory_order_relaxed);
      const guint64 qsrc_c  = g_diag[qi + sr_ids[k]].count.load(std::memory_order_relaxed);
      const gint64 qsink_lu = g_diag[qi + sk_ids[k]].last_us.load(std::memory_order_relaxed);
      const gint64 qsrc_lu  = g_diag[qi + sr_ids[k]].last_us.load(std::memory_order_relaxed);
      fprintf(stderr,
              "[diagq] g%d %s level=%u/%llu/%llu task=%s "
              "sink=%d/%d/%d src=%d/%d/%d q.sink=%llu/%lldms q.src=%llu/%lldms\n",
              g, GST_ELEMENT_NAME(q),
              lvl_b, static_cast<unsigned long long>(lvl_bytes),
              static_cast<unsigned long long>(lvl_time), ts,
              sk ? gst_pad_is_active(sk) : -1,
              sk ? gst_pad_is_linked(sk) : -1,
              sk ? (GST_PAD_IS_FLUSHING(sk) ? 1 : 0) : -1,
              sp ? gst_pad_is_active(sp) : -1,
              sp ? gst_pad_is_linked(sp) : -1,
              sp ? (GST_PAD_IS_FLUSHING(sp) ? 1 : 0) : -1,
              static_cast<unsigned long long>(qsink_c),
              static_cast<long long>(qsink_lu ? (now - qsink_lu) / 1000 : -1),
              static_cast<unsigned long long>(qsrc_c),
              static_cast<long long>(qsrc_lu ? (now - qsrc_lu) / 1000 : -1));
      if (sp) gst_object_unref(sp);
      if (sk) gst_object_unref(sk);
    }
  }
  return G_SOURCE_CONTINUE;
}

void DiagEnableIfRequested() {
  if (g_diag_enabled) return;
  if (!g_getenv("AIBOX_FACE_DIAG_1HZ")) return;
  g_diag_enabled = true;
  const char* names[kDiagPointsPerGroup] = {
      "lm.src", "qfi.sink", "qfi.src", "fi.sink", "fi.src",
      "qfo.sink", "qfo.src", "ov.sink", "ov.src", "tee_post",
      "qdm.sink", "qdm.src"};
  for (int i = 0; i < kDiagPointsPerGroup; ++i) {
    if (i < 12) g_diag_name[i] = names[i];
    else if (i < D_ENC_SINK_BASE) g_diag_name[i] = "dm.src";
    else if (i < D_ENC_SRC_BASE) g_diag_name[i] = "enc.sink";
    else g_diag_name[i] = "enc.src";
  }
  g_diag_timer = g_timeout_add_seconds(1, DiagTimerCb, nullptr);
  fprintf(stderr, "[diag1hz] enabled\n");
}

static int DiagId(int id) { return id + g_diag_group * kDiagGroupOffset; }

void DiagAttach(GstElement* e, const char* pad_name, int id) {
  if (!g_diag_enabled || !e) return;
  GstPad* p = gst_element_get_static_pad(e, pad_name);
  if (!p) return;
  gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, DiagBufProbe,
                    GINT_TO_POINTER(DiagId(id)), nullptr);
  gst_object_unref(p);
}

void DiagAttachPad(GstPad* p, int id) {
  if (!g_diag_enabled || !p) return;
  gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, DiagBufProbe,
                    GINT_TO_POINTER(DiagId(id)), nullptr);
}

void DiagOverlaySinkTick(int group) {
  if (!g_diag_enabled || group < 0) return;
  const int id = group * kDiagGroupOffset + D_OV_SINK;
  g_diag[id].count.fetch_add(1, std::memory_order_relaxed);
  g_diag[id].last_us.store(g_get_monotonic_time(),
                           std::memory_order_relaxed);
}

void DiagOverlaySetUs(gint64 us) {
  if (!g_diag_enabled) return;
  std::lock_guard<std::mutex> lk(g_setring_mtx);
  g_setring[g_setring_pos] = us;
  g_setring_pos = (g_setring_pos + 1) % g_setring.size();
  if (g_setring_n < static_cast<int>(g_setring.size())) ++g_setring_n;
}

/* Diagnostic-only flow taps T0-T3 (expert round 3). Off in production. */
bool FlowTapEnabled() {
  const char* e = g_getenv("AIBOX_FLOWTAP");
  return e && e[0] == '1';
}
}  // namespace

static GstPadProbeReturn
OnFaceOverlaySinkProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

static bool AttachOverlayMaskProbe(GstElement* overlay, int diag_group,
                                   std::string* err) {
  GstPad* ov_sink = gst_element_get_static_pad(overlay, "sink");
  if (!ov_sink) {
    if (err) *err = "overlay sink pad missing";
    return false;
  }
  auto* ov_ctx = new (std::nothrow) FaceOverlayProbeCtx{};
  if (!ov_ctx) {
    gst_object_unref(ov_sink);
    if (err) *err = "allocate FaceOverlayProbeCtx failed";
    return false;
  }
  ov_ctx->overlay = overlay;
  ov_ctx->diag_group = diag_group;
  const gulong probe_id = gst_pad_add_probe(
      ov_sink, GST_PAD_PROBE_TYPE_BUFFER, OnFaceOverlaySinkProbe, ov_ctx,
      FaceOverlayProbeCtxFree);
  gst_object_unref(ov_sink);
  if (probe_id == 0) {
    FaceOverlayProbeCtxFree(ov_ctx);
    if (err) *err = "add overlay sink probe failed";
    return false;
  }
  return true;
}

// Smooth-first / fail-open overlay mask policy (dual meta:
// GstFaceResultStateMeta decides boxes/clear; GstFaceDetectionMeta carries
// the boxes). UNAVAILABLE/STALE/invalid states omit face boxes; detection
// ROI boxes still draw. g_object_set("masks") is deduplicated.
static GstPadProbeReturn
OnFaceOverlaySinkProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  auto* ctx = static_cast<FaceOverlayProbeCtx*>(user_data);
  GstElement* ov = ctx->overlay;
  LogFaceSyncTriple("qtivoverlay.sink", buf);
  DiagOverlaySinkTick(ctx->diag_group);  // ctx group, NOT the global

  static GType face_api = G_TYPE_INVALID;
  static GType state_api = G_TYPE_INVALID;
  if (face_api == G_TYPE_INVALID)
    face_api = g_type_from_name("GstFaceDetectionMetaAPI");
  if (state_api == G_TYPE_INVALID)
    state_api = g_type_from_name("GstFaceResultStateMetaAPI");

  GstFaceResultStateMeta* sm = nullptr;
  gboolean abi_ok = FALSE, size_ok = FALSE;
  if (state_api != G_TYPE_INVALID) {
    sm = reinterpret_cast<GstFaceResultStateMeta*>(
        gst_buffer_get_meta(buf, state_api));
    if (sm) {
      /* gst_meta_get_info() takes the impl NAME, not the instance —
       * the MetaInfo lives in meta.info */
      const GstMetaInfo* mi = sm->meta.info;
      abi_ok = sm->abi_version == FACE_RESULT_STATE_META_ABI_VERSION;
      size_ok = mi && mi->size >= sizeof(GstFaceResultStateMeta);
    }
  }
  const GstFaceDetectionMeta* dm =
      (face_api != G_TYPE_INVALID)
          ? reinterpret_cast<const GstFaceDetectionMeta*>(
                gst_buffer_get_meta(buf, face_api))
          : nullptr;

  // Pure policy decision (unit-tested in face_async_tests)
  aibox::face::MaskPolicyInput pin;
  pin.state_meta_present = (sm != nullptr);
  pin.abi_version_ok = abi_ok;
  pin.meta_size_ok = size_ok;
  pin.result_status = (sm && abi_ok && size_ok)
      ? sm->result_status : aibox::face::kMaskStatusUnavailable;
  pin.det_meta_present = (dm != nullptr);
  pin.n_faces = dm ? dm->n_faces : 0u;
  pin.n_faces_over_max =
      (dm && dm->n_faces > FACE_META_MAX_FACES) ? 1u : 0u;
  pin.any_valid_bbox = FALSE;
  if (dm) {
    const guint n = std::min<guint>(dm->n_faces, FACE_META_MAX_FACES);
    for (guint i = 0; i < n; ++i) {
      const auto& f = dm->faces[i];
      if (f.x2 - f.x1 > 0 && f.y2 - f.y1 > 0) {
        pin.any_valid_bbox = TRUE;
        break;
      }
    }
  }
  const aibox::face::MaskDecision d = aibox::face::DecideMask(pin);

  // Production path (native-meta, unconditional): qtivoverlay draws the ROI
  // meta injected by the postprocess plugin. All dynamic overlay properties
  // (bboxes / strings / masks) were REMOVED: their setters race inside
  // qtivoverlay 1.8.1 when probes run concurrently on several cameras
  // (gdb: SIGSEGV in gst_extract_strings on a metamux push thread, followed
  // by gst_mini_object_unref >0 failures and heap corruption). Face white
  // masking is owned by the self-owned component, never by qtivoverlay.
  // C-group evidence collection (read-only, AIBOX_FACE_BUFDIAG=1): report the
  // buffer contract at the drawing point so the self-owned white-mask
  // component can be designed against real caps / memory / stride facts.
  if (g_getenv("AIBOX_FACE_BUFDIAG")) {
    static gint64 last_bufdiag_us = 0;
    const gint64 bnow = g_get_monotonic_time();
    if (bnow - last_bufdiag_us > 1000000) {
      last_bufdiag_us = bnow;
      GstCaps* caps = gst_pad_get_current_caps(pad);
      gchar* caps_str = caps ? gst_caps_to_string(caps) : g_strdup("(null)");
      const gchar* feat = "none";
      if (caps) {
        GstCapsFeatures* f = gst_caps_get_features(caps, 0);
        if (f && !gst_caps_features_is_any(f))
          feat = gst_caps_features_to_string(f);
      }
      GstVideoMeta* vmeta = gst_buffer_get_video_meta(buf);
      fprintf(stderr,
              "[FACE-BUF] caps=%s | features=%s | writable=%d n_mem=%u "
              "refcount=%d vmeta=%d stride0=%d offset0=%d\n",
              caps_str, feat,
              gst_buffer_is_writable(buf) ? 1 : 0,
              (guint)gst_buffer_n_memory(buf),
              GST_MINI_OBJECT_REFCOUNT_VALUE(GST_MINI_OBJECT_CAST(buf)),
              vmeta ? 1 : 0,
              vmeta ? vmeta->stride[0] : -1,
              vmeta ? (int)vmeta->offset[0] : -1);
      g_free(caps_str);
      if (caps) gst_caps_unref(caps);
    }
  }

  // Audit counters (B group): frame / face-meta accounting with 1 Hz
  // rate-limited output, so face inference can be proven to keep working
  // without any per-frame logging. Counters are diagnostics only.
  static gint64 last_native_log_us = 0;
  static guint64 s_frames = 0, s_face_frames = 0, s_face_metas = 0;
  static gint64 s_last_face_us = 0;
  const gint64 now = g_get_monotonic_time();
  ++s_frames;
  const guint n_faces_now = dm ? dm->n_faces : 0u;
  if (n_faces_now > 0) {
    ++s_face_frames;
    s_face_metas += n_faces_now;
    s_last_face_us = now;
  }
  if (now - last_native_log_us > 1000000) {
    last_native_log_us = now;
    guint nroi = 0;
    gpointer nst = nullptr;
    while (GstMeta* m = gst_buffer_iterate_meta(buf, &nst)) {
      if (m->info->api == GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE) ++nroi;
    }
    const long long ago_ms =
        s_last_face_us ? (long long)((now - s_last_face_us) / 1000) : -1;
    fprintf(stderr,
            "[OV-NATIVE] rois=%u faces=%u decision=%d | frames=%llu "
            "face_frames=%llu face_metas=%llu last_face_ago_ms=%lld "
            "(no property set)\n",
            nroi, n_faces_now, static_cast<int>(d),
            (unsigned long long)s_frames, (unsigned long long)s_face_frames,
            (unsigned long long)s_face_metas, ago_ms);
  }
  (void)sm;
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
OnFrameTagProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  (void)pad;
  auto *ctx = static_cast<FrameTagCtx *>(user_data);
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  ctx->frame_counter++;

  buf = gst_buffer_make_writable(buf);
  if (!buf) return GST_PAD_PROBE_OK;
  GST_PAD_PROBE_INFO_DATA(info) = buf;

  static GType api_type = 0;
  if (G_UNLIKELY(api_type == 0))
    api_type = g_type_from_name("GstAiboxFrameMetaAPI");

  GstAiboxFrameMeta *m = nullptr;
  if (api_type != 0)
    m = (GstAiboxFrameMeta *)gst_buffer_get_meta(buf, api_type);

  if (m) {
    g_free(m->camera_id);
    m->camera_id = g_strdup(ctx->camera_id);
    m->frame_id  = ctx->frame_counter;
  } else {
    m = add_aibox_frame_meta_dynamic(buf, ctx->camera_id, ctx->frame_counter);
  }
  if (m && ctx->batch_group_id) {
    g_free(m->batch_group_id);
    m->batch_group_id  = g_strdup(ctx->batch_group_id);
    m->batch_stream_id = ctx->batch_stream_id;
  }
#ifdef HAVE_DETECTION_NATS
  if (auto* tracker = aibox::detection::CaptureTimestampTracker::GetProcessInstance()) {
    tracker->Record(ctx->camera_id ? ctx->camera_id : "", ctx->frame_counter,
                    static_cast<uint64_t>(g_get_real_time() / 1000));
  }
#endif
  return GST_PAD_PROBE_OK;
}

static bool NowebFpsEnabled(const GstPipelineBuildOptions& opts) {
  return opts.enable_yolo_stages && !opts.enable_webrtc &&
         !opts.enable_annotated_rtsp && !opts.enable_mosaic_recording;
}

static std::string FpsSafeName(const std::string& id) {
  std::string s = id;
  for (auto& ch : s)
    if (ch == '-' || ch == ':' || ch == '/') ch = '_';
  return s;
}

// ===================================================================
// OUT frame count probe (on fakesink sink pad, noweb FPS path)
// ===================================================================
struct FrameCountOutCtx {
  std::string camera_id;
  guint64     out_count = 0;
};

static GstPadProbeReturn
OnFrameOutProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  (void)pad;
  if (GST_PAD_PROBE_INFO_BUFFER(info))
    static_cast<FrameCountOutCtx *>(user_data)->out_count++;
  return GST_PAD_PROBE_OK;
}

// ===================================================================
// Stage latency probes — AIBOX_STAGE_LAT=1 only; zero effect otherwise.
// ===================================================================
struct StageLatCtx {
  std::string name;
  std::mutex  mu;
  guint64     count  = 0;
  gint64      sum_us = 0;
  gint64      max_us = 0;
  gint64      t_in   = 0;
};

static bool StageLatEnabled() {
  const char* e = g_getenv("AIBOX_STAGE_LAT");
  return e && e[0] == '1';
}

static GstPadProbeReturn
OnStageInProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  (void)pad;
  if (!GST_PAD_PROBE_INFO_BUFFER(info)) return GST_PAD_PROBE_OK;
  auto* ctx = static_cast<StageLatCtx*>(user_data);
  std::lock_guard<std::mutex> lock(ctx->mu);
  ctx->t_in = g_get_monotonic_time();
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
OnStageOutProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  (void)pad;
  if (!GST_PAD_PROBE_INFO_BUFFER(info)) return GST_PAD_PROBE_OK;
  auto* ctx = static_cast<StageLatCtx*>(user_data);
  const gint64 now = g_get_monotonic_time();
  std::lock_guard<std::mutex> lock(ctx->mu);
  if (ctx->t_in <= 0) return GST_PAD_PROBE_OK;
  const gint64 delta = now - ctx->t_in;
  ctx->t_in = 0;
  if (delta < 0) return GST_PAD_PROBE_OK;
  ctx->count++;
  ctx->sum_us += delta;
  if (delta > ctx->max_us) ctx->max_us = delta;
  return GST_PAD_PROBE_OK;
}

static void AttachStageLatProbes(GstElement* elem, const std::string& name,
                                 RuntimeRegistry* reg) {
  if (!StageLatEnabled() || !elem || !reg) return;
  auto* ctx = new StageLatCtx();
  ctx->name = name;
  GstPad* sink = gst_element_get_static_pad(elem, "sink");
  GstPad* src  = gst_element_get_static_pad(elem, "src");
  if (sink) {
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, OnStageInProbe, ctx,
                      nullptr);
    gst_object_unref(sink);
  }
  if (src) {
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, OnStageOutProbe, ctx,
                      nullptr);
    gst_object_unref(src);
  }
  reg->stage_lat_ctx.push_back(ctx);
  fprintf(stderr, "[STAGE_LAT] attached elem=%s\n", name.c_str());
}

void PrintStageLatSummary(RuntimeRegistry& reg) {
  if (reg.stage_lat_ctx.empty()) return;
  std::ofstream out("./output/stage_lat_summary.txt");
  auto emit = [&](const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
    if (out.is_open()) out << line << "\n";
  };
  emit("=== STAGE_LAT Summary ===");
  char hdr[160];
  std::snprintf(hdr, sizeof(hdr), "%-48s %8s %10s %10s",
                "elem", "n", "mean_ms", "max_ms");
  emit(hdr);
  for (void* ptr : reg.stage_lat_ctx) {
    auto* ctx = static_cast<StageLatCtx*>(ptr);
    std::lock_guard<std::mutex> lock(ctx->mu);
    const double mean_ms =
        ctx->count ? (ctx->sum_us / 1000.0) / static_cast<double>(ctx->count)
                   : 0.0;
    const double max_ms = ctx->max_us / 1000.0;
    char row[192];
    std::snprintf(row, sizeof(row), "%-48s %8llu %10.2f %10.2f",
                  ctx->name.c_str(),
                  static_cast<unsigned long long>(ctx->count), mean_ms, max_ms);
    emit(row);
  }
}

// ===================================================================
// FPS CSV output (shared across all fpsdisplaysink instances)
// ===================================================================
struct FpsCsvCtx {
  std::string camera_id;
};

static std::mutex g_fps_csv_mutex;
static std::ofstream g_fps_csv;

void OpenFpsCsv(const std::string& path) {
  g_fps_csv.open(path);
  if (g_fps_csv.is_open())
    g_fps_csv << "timestamp_ms,camera_id,current_fps,min_fps,max_fps\n";
}

void CloseFpsCsv() {
  if (g_fps_csv.is_open()) g_fps_csv.close();
}

void AttachFrameCountOutProbe(GstElement* elem,
                              const std::string& camera_id,
                              RuntimeRegistry* reg) {
  if (!elem || !reg) return;
  GstPad* src = gst_element_get_static_pad(elem, "src");
  if (!src) {
    fprintf(stderr, "[OUTCNT] no src pad on %s (camera %s)\n",
            GST_ELEMENT_NAME(elem), camera_id.c_str());
    return;
  }
  auto* ctx = new FrameCountOutCtx{camera_id, 0};
  gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, OnFrameOutProbe, ctx,
                    nullptr);
  gst_object_unref(src);
  reg->frame_count_ctx_out.push_back(ctx);
}

void SnapshotFrameCounters(RuntimeRegistry& reg) {
  aibox::pipeline::frame_count_snapshots.clear();

  std::unordered_map<std::string, guint64> in_map;
  for (void *ptr : reg.frame_count_ctx_in) {
    auto *ctx = static_cast<FrameTagCtx *>(ptr);
    std::string cid = ctx->camera_id ? ctx->camera_id : "?";
    in_map[cid] = ctx->frame_counter;
  }

  std::unordered_map<std::string, guint64> out_map;
  for (void *ptr : reg.frame_count_ctx_out) {
    auto *ctx = static_cast<FrameCountOutCtx *>(ptr);
    out_map[ctx->camera_id] = ctx->out_count;
  }

  for (auto &kv : in_map) {
    FrameCountSnapshot s;
    s.camera_id  = kv.first;
    s.frames_in  = kv.second;
    s.frames_out = out_map[kv.first];
    frame_count_snapshots.push_back(std::move(s));
  }
  for (auto &kv : out_map) {
    if (in_map.find(kv.first) == in_map.end()) {
      FrameCountSnapshot s;
      s.camera_id  = kv.first;
      s.frames_out = kv.second;
      frame_count_snapshots.push_back(std::move(s));
    }
  }

  std::sort(frame_count_snapshots.begin(), frame_count_snapshots.end(),
    [](const FrameCountSnapshot &a, const FrameCountSnapshot &b) {
      return a.camera_id < b.camera_id;
    });
}

static void
OnFpsMeasurement(GstElement *fps, gdouble current_fps,
                 gdouble min_fps, gdouble max_fps, gpointer user_data)
{
  (void)fps;
  auto *ctx = static_cast<FpsCsvCtx *>(user_data);
  gint64 now_ms = g_get_monotonic_time() / 1000;

  fprintf(stderr, "[FPS] camera=%s fps=%.1f\n",
          ctx->camera_id.c_str(), current_fps);

  {
    std::lock_guard<std::mutex> lock(g_fps_csv_mutex);
    if (g_fps_csv.is_open()) {
      g_fps_csv << now_ms << ","
                << ctx->camera_id << ","
                << current_fps << ","
                << min_fps << ","
                << max_fps << "\n";
      g_fps_csv.flush();
    }
  }
}

struct BatchMaskCtx {
  std::string label;
  guint       expected_mask = 0;
  std::shared_ptr<std::atomic<guint64>> total;
  std::shared_ptr<std::atomic<guint64>> partial;
};

static constexpr int kR0FirstN = 16;

static gchar*
r0_caps_str(GstPad* pad) {
  if (!pad) return g_strdup("(no-pad)");
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  if (!caps) return g_strdup("(no-caps)");
  gchar* s = gst_caps_to_string(caps);
  gst_caps_unref(caps);
  return s ? s : g_strdup("(caps-to-string-fail)");
}

static gchar*
r0_segment_str(GstPad* pad) {
  if (!pad) return g_strdup("(no-pad)");
  GstEvent* ev = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
  if (!ev) return g_strdup("(no-segment)");
  const GstSegment* seg = nullptr;
  gst_event_parse_segment(ev, &seg);
  gchar* s = nullptr;
  if (seg) {
    s = g_strdup_printf("fmt=%s start=%" GST_TIME_FORMAT " stop=%" GST_TIME_FORMAT
                        " time=%" GST_TIME_FORMAT " base=%" GST_TIME_FORMAT,
                        gst_format_get_name(seg->format),
                        GST_TIME_ARGS(seg->start), GST_TIME_ARGS(seg->stop),
                        GST_TIME_ARGS(seg->time), GST_TIME_ARGS(seg->base));
  } else {
    s = g_strdup("(segment-parse-fail)");
  }
  gst_event_unref(ev);
  return s;
}

struct R0ParentDump {
  int count = 0;
  int with_framemeta = 0;
  guint stream_seen = 0;
  bool dup_stream = false;
  bool dup_camera = false;
  gchar* cameras[4] = {};
};

static gboolean
r0_foreach_parent(GstBuffer* /*buf*/, GstMeta** meta, gpointer user_data) {
  auto* d = static_cast<R0ParentDump*>(user_data);
  if (!meta || !*meta || !(*meta)->info) return TRUE;
  const gchar* api_name = g_type_name((*meta)->info->api);
  if (g_strcmp0(api_name, "GstParentBufferMetaAPI") != 0) return TRUE;
  struct LocalParent {
    GstMeta meta;
    GstBuffer* buffer;
  };
  auto* pm = reinterpret_cast<LocalParent*>(*meta);
  GstBuffer* parent = pm->buffer;
  d->count++;
  static GType fm_api = 0;
  if (G_UNLIKELY(fm_api == 0))
    fm_api = g_type_from_name("GstAiboxFrameMetaAPI");
  GstAiboxFrameMeta* fm =
      (parent && fm_api != 0)
          ? (GstAiboxFrameMeta*)gst_buffer_get_meta(parent, fm_api)
          : nullptr;
  const gchar* cam = (fm && fm->camera_id) ? fm->camera_id : "(no-fm)";
  guint sid = fm ? fm->batch_stream_id : GST_AIBOX_FRAME_META_BATCH_INVALID_STREAM_ID;
  guint64 fid = fm ? fm->frame_id : 0;
  if (fm) d->with_framemeta++;
  if (sid < 32) {
    if (d->stream_seen & (1u << sid)) d->dup_stream = true;
    d->stream_seen |= (1u << sid);
  }
  for (int i = 0; i < d->count - 1 && i < 4; ++i) {
    if (d->cameras[i] && cam && g_strcmp0(d->cameras[i], cam) == 0)
      d->dup_camera = true;
  }
  if (d->count <= 4) d->cameras[d->count - 1] = g_strdup(cam);
  fprintf(stderr,
          "[R0_PARENT] i=%d parent=%p pts=%" GST_TIME_FORMAT
          " size=%zu nmem=%u cam=%s stream=%u frame=%lu fm=%d\n",
          d->count - 1, static_cast<void*>(parent),
          GST_TIME_ARGS(parent ? GST_BUFFER_PTS(parent) : GST_CLOCK_TIME_NONE),
          parent ? gst_buffer_get_size(parent) : 0,
          parent ? gst_buffer_n_memory(parent) : 0, cam, sid,
          static_cast<unsigned long>(fid), fm ? 1 : 0);
  return TRUE;
}

static void
r0_dump_src_buffer(GstPad* pad, GstBuffer* buf, const char* label, guint64 seq) {
  gchar* caps = r0_caps_str(pad);
  gchar* seg = r0_segment_str(pad);
  const guint64 raw_off = GST_BUFFER_OFFSET(buf);
  const guint64 raw_end = GST_BUFFER_OFFSET_END(buf);
  const guint mask = (raw_off == GST_BUFFER_OFFSET_NONE)
                         ? 0u
                         : static_cast<guint>(raw_off & 0xFFFFFFFFu);
  gboolean has_prot = FALSE;
  GType prot = g_type_from_name("GstProtectionMetaAPI");
  if (prot != 0 && gst_buffer_get_meta(buf, prot)) has_prot = TRUE;
  fprintf(stderr,
          "[R0_SRC] seq=%lu label=%s offset=0x%llx offset_end=0x%llx "
          "mask=0x%x pts=%" GST_TIME_FORMAT " size=%zu nmem=%u prot=%d "
          "caps=%s segment=%s\n",
          static_cast<unsigned long>(seq), label,
          static_cast<unsigned long long>(raw_off),
          static_cast<unsigned long long>(raw_end), mask,
          GST_TIME_ARGS(GST_BUFFER_PTS(buf)), gst_buffer_get_size(buf),
          gst_buffer_n_memory(buf), has_prot ? 1 : 0, caps, seg);
  g_free(caps);
  g_free(seg);
  R0ParentDump dump;
  gst_buffer_foreach_meta(buf, r0_foreach_parent, &dump);
  const bool full = (mask == 0x3);
  const bool map_ok = full && dump.count == 2 && dump.with_framemeta == 2 &&
                      dump.stream_seen == 0x3 && !dump.dup_stream &&
                      !dump.dup_camera;
  fprintf(stderr,
          "[R0_MAP] seq=%lu mask=0x%x parents=%d framemeta=%d streams=0x%x "
          "dup_stream=%d dup_cam=%d map_ok=%d\n",
          static_cast<unsigned long>(seq), mask, dump.count, dump.with_framemeta,
          dump.stream_seen, dump.dup_stream ? 1 : 0, dump.dup_camera ? 1 : 0,
          map_ok ? 1 : 0);
  for (int i = 0; i < 4; ++i) g_free(dump.cameras[i]);
}

struct R0SinkCtx {
  std::string label;
  std::shared_ptr<std::atomic<guint64>> seq =
      std::make_shared<std::atomic<guint64>>(0);
};

static GstPadProbeReturn
OnR0SinkProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  auto* ctx = static_cast<R0SinkCtx*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf || !ctx) return GST_PAD_PROBE_OK;
  const guint64 seq = ctx->seq->fetch_add(1) + 1;
  if (seq > static_cast<guint64>(kR0FirstN)) return GST_PAD_PROBE_OK;
  static GType fm_api = 0;
  if (G_UNLIKELY(fm_api == 0))
    fm_api = g_type_from_name("GstAiboxFrameMetaAPI");
  GstAiboxFrameMeta* fm =
      (fm_api != 0) ? (GstAiboxFrameMeta*)gst_buffer_get_meta(buf, fm_api)
                    : nullptr;
  gchar* caps = r0_caps_str(pad);
  gchar* seg = r0_segment_str(pad);
  fprintf(stderr,
          "[R0_SINK] seq=%lu label=%s cam=%s stream=%u frame=%lu "
          "pts=%" GST_TIME_FORMAT " dur=%" GST_TIME_FORMAT
          " size=%zu nmem=%u caps=%s segment=%s\n",
          static_cast<unsigned long>(seq), ctx->label.c_str(),
          (fm && fm->camera_id) ? fm->camera_id : "(no-fm)",
          fm ? fm->batch_stream_id : GST_AIBOX_FRAME_META_BATCH_INVALID_STREAM_ID,
          static_cast<unsigned long>(fm ? fm->frame_id : 0),
          GST_TIME_ARGS(GST_BUFFER_PTS(buf)),
          GST_TIME_ARGS(GST_BUFFER_DURATION(buf)), gst_buffer_get_size(buf),
          gst_buffer_n_memory(buf), caps, seg);
  g_free(caps);
  g_free(seg);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
OnBatchSrcProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
  auto* ctx = static_cast<BatchMaskCtx*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  if (ctx->total) ctx->total->fetch_add(1);
  const guint64 seq = ctx->total ? ctx->total->load() : 0;
  if (seq <= static_cast<guint64>(kR0FirstN))
    r0_dump_src_buffer(pad, buf, ctx->label.c_str(), seq);

  const guint64 raw_off = GST_BUFFER_OFFSET(buf);
  if (raw_off == GST_BUFFER_OFFSET_NONE) {
    fprintf(stderr, "[R0_SRC] seq=%lu OFFSET_NONE (not a full-B2 mask)\n",
            static_cast<unsigned long>(seq));
    return GST_PAD_PROBE_OK;
  }
  const guint present = static_cast<guint>(raw_off & 0xFFFFFFFFu);
  if ((present & ctx->expected_mask) != ctx->expected_mask) {
    if (ctx->partial) ctx->partial->fetch_add(1);
    fprintf(stderr,
            "[BATCH_PARTIAL] %s channel_mask=0x%x expected=0x%x — pass downstream "
            "(qtimldemux emits present src_N; bypass is pre-batch tee)\n",
            ctx->label.c_str(), present, ctx->expected_mask);
  }
  return GST_PAD_PROBE_OK;
}

static void
install_batch_src_probe(GstElement* batch, const std::string& label,
                        guint expected_mask, BatchGroupRuntime* br) {
  GstPad* pad = gst_element_get_static_pad(batch, "src");
  if (!pad) return;
  auto* ctx = new BatchMaskCtx();
  ctx->label = label;
  ctx->expected_mask = expected_mask;
  if (br) {
    ctx->total = br->batch_total;
    ctx->partial = br->batch_partial;
  }
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                    OnBatchSrcProbe, ctx,
                    [](gpointer d) { delete static_cast<BatchMaskCtx*>(d); });
  gst_object_unref(pad);
}

static void
trigger_aibox_frame_meta_registration(void)
{
  static bool done = false;
  if (done) return;

  /* Single-ABI plugin owns the only GstAiboxFrameMeta registration. */
  GstPlugin *plugin = gst_plugin_load_by_name("aiboxframemeta");
  if (!plugin) {
    g_error("aiboxframemeta plugin missing or failed to load "
            "(check GST_PLUGIN_PATH contains libgstaiboxframemeta.so)");
    return;
  }

  if (!gst_meta_get_info("GstAiboxFrameMeta")) {
    gst_object_unref(plugin);
    g_error("gstaiboxframemeta: GstAiboxFrameMeta registration failed");
    return;
  }

  g_print("[init] GstAiboxFrameMeta registration triggered (aiboxframemeta)\n");
  gst_object_unref(plugin);
  done = true;
}

// ===================================================================
// Constructor
// ===================================================================
GstPipelineBuilder::GstPipelineBuilder(GstPipelineBuildOptions opts)
    : opts_(std::move(opts)) {}

// ===================================================================
// ValidateFactories
// ===================================================================
Status GstPipelineBuilder::ValidateFactories(const ExecutionPlan& plan) const {
  static const std::vector<std::string> kRequired = {
    "rtspsrc", "rtph264depay", "h264parse", "v4l2h264dec",
    "rtph265depay", "h265parse", "v4l2h265dec",
    "openh264dec", "videoconvert",
    "capsfilter", "queue", "aiboxtimemux", "fakesink", "appsink",
    "tee", "qtimetamux"
  };
  for (auto& f : kRequired) {
    if (!FactoryExists(f))
      return Status::Error("GST_FACTORY_NOT_FOUND", f);
  }
  // Phase 5 recording elements (only validated if recording is requested)
  if (opts_.enable_mosaic_recording) {
    static const std::vector<std::string> kRecording = {
      "aiboxstreamdemux", "qtivoverlay", "qtivcomposer",
      "v4l2h264enc", "h264parse", "matroskamux", "filesink"
    };
    for (auto& f : kRecording) {
      if (!FactoryExists(f))
        return Status::Error("RECORDING_FACTORY_NOT_FOUND", f);
    }
  }
  // WebRTC annotated branch: aiboxstreamdemux (zero-copy default) or
  // aiboxcamsplit when AIBOX_WEBRTC_SPLITTER=camsplit.
  // Stage0 caps probe (AIBOX_CAPS_PROBE=1) skips splitter entirely.
  if (opts_.enable_webrtc) {
    const char* caps_probe = g_getenv("AIBOX_CAPS_PROBE");
    const bool stage0 = (caps_probe && caps_probe[0] == '1');
    if (!stage0) {
      const char* sp = g_getenv("AIBOX_WEBRTC_SPLITTER");
      const bool use_camsplit = (sp && g_strcmp0(sp, "camsplit") == 0);
      static const std::vector<std::string> kWebrtcBase = {
        "qtivoverlay", "videoscale", "v4l2h264enc", "h264parse"
      };
      const std::string splitter =
          use_camsplit ? "aiboxcamsplit" : "aiboxstreamdemux";
      if (!FactoryExists(splitter))
        return Status::Error("WEBRTC_FACTORY_NOT_FOUND", splitter);
      for (auto& f : kWebrtcBase) {
        if (!FactoryExists(f))
          return Status::Error("WEBRTC_FACTORY_NOT_FOUND", f);
      }
    }
  }
  if (ShouldDispatchBatch(plan)) {
    static const std::vector<std::string> kBatch = {
      "aiboxbatchsplit", "qtibatch", "qtimldemux",
      "qtimlvconverter", "qtimlqnn", "qtimlpostprocess_roi"
    };
    for (auto& f : kBatch) {
      if (!FactoryExists(f))
        return Status::Error("BATCH_FACTORY_NOT_FOUND", f);
    }
  }
  if (NowebFpsEnabled(opts_)) {
    if (!FactoryExists("fpsdisplaysink"))
      return Status::Error("NOWEB_FPS_FACTORY_NOT_FOUND", "fpsdisplaysink");
    if (!ShouldDispatchBatch(plan) && !FactoryExists("aiboxstreamdemux"))
      return Status::Error("NOWEB_FPS_FACTORY_NOT_FOUND", "aiboxstreamdemux");
  }
  return Status::Ok();
}

// ===================================================================
// BuildMux
// ===================================================================
Status GstPipelineBuilder::BuildMux(const TimeMuxGroupPlan& mux_plan,
                                     GstElement* pipeline,
                                     RuntimeRegistry* reg) const {
  std::string err;
  GstElement* mux = MakeElement(mux_plan.mux_factory, mux_plan.mux_name, &err);
  if (!mux) return Status::Error("MUX_CREATE_FAILED", err);

  // Properties (from MuxSchedulerPlan)
  g_object_set(mux,
    "max-sinks",    static_cast<guint>(mux_plan.mux_scheduler.max_sinks),
    "wait-timeout-ms", static_cast<guint>(mux_plan.mux_scheduler.wait_timeout_ms),
    "attach-meta",  mux_plan.mux_scheduler.attach_meta ? TRUE : FALSE,
    "mux-group-id", mux_plan.time_mux_group_id.c_str(),
    "algorithm-group-id", mux_plan.parent_algorithm_group_id.c_str(),
    nullptr);
  gst_util_set_object_arg(G_OBJECT(mux), "drop-policy", "latest");
  gst_util_set_object_arg(G_OBJECT(mux), "caps-mode",
                          mux_plan.mux_scheduler.caps_mode.c_str());
  if (mux_plan.mux_scheduler.caps_mode == "canonical_raw") {
    GstCaps* group_caps = gst_caps_from_string(
        mux_plan.mux_scheduler.group_caps.c_str());
    if (!group_caps || !gst_caps_is_fixed(group_caps) ||
        gst_caps_get_size(group_caps) != 1) {
      if (group_caps) gst_caps_unref(group_caps);
      return Status::Error("TIMEMUX_GROUP_CAPS_INVALID",
                           "group_caps must be one fixed video/x-raw caps");
    }
    const GstStructure* structure = gst_caps_get_structure(group_caps, 0);
    const gchar* format = gst_structure_get_string(structure, "format");
    const gchar* interlace = gst_structure_get_string(structure, "interlace-mode");
    gint width = 0;
    gint height = 0;
    if (g_strcmp0(gst_structure_get_name(structure), "video/x-raw") != 0 ||
        !format || !interlace ||
        !gst_structure_get_int(structure, "width", &width) ||
        !gst_structure_get_int(structure, "height", &height) ||
        width <= 0 || height <= 0 || g_strcmp0(interlace, "progressive") != 0) {
      gst_caps_unref(group_caps);
      return Status::Error("TIMEMUX_GROUP_CAPS_INVALID",
                           "group_caps requires video/x-raw format, positive width/height, and progressive interlace-mode");
    }
    g_object_set(mux, "group-caps", group_caps, nullptr);
    gst_caps_unref(group_caps);
  }

  gst_bin_add(GST_BIN(pipeline), mux);

  reg->mux_by_group[mux_plan.time_mux_group_id] = mux;
  reg->mux_src_pad_by_group[mux_plan.time_mux_group_id] =
      gst_element_get_static_pad(mux, "src");
  return Status::Ok();
}

// ===================================================================
// BuildDecodeBranch
// ===================================================================
Status GstPipelineBuilder::BuildDecodeBranch(
    const StreamInputPlan& input,
    const TimeMuxGroupPlan& mux_plan,
    GstElement* pipeline, GstElement* mux,
    RuntimeRegistry* reg) const {

  std::string err;

  // 硬解 v4l2h264dec/v4l2h265dec 直出 NV12; 软解(openh264dec)只出 I420, 需 videoconvert。
  // 解码器类型由 config 快照 decode.decoder 决定。
  const bool is_hw_decoder = IsV4l2HwDecoder(input.decoder_element);

  // --- create elements using names from Plan ---
  GstElement* src    = MakeElement(input.source_element, input.source_name, &err);
  GstElement* depay  = MakeElement(input.depay_element,  input.depay_name, &err);
  GstElement* parse  = MakeElement(input.parse_element,  input.parse_name, &err);
  GstElement* decoder= MakeElement(input.decoder_element,input.decoder_name,&err);
  GstElement* conv   = is_hw_decoder ? nullptr
                                     : MakeElement("videoconvert", "conv_" + input.camera_id, &err);
  GstElement* caps   = MakeElement(input.capsfilter_element,input.capsfilter_name,&err);
  GstElement* queue  = MakeElement(input.queue_element,  input.queue_name, &err);

  if (!src || !depay || !parse || !decoder || !caps || !queue ||
      (!is_hw_decoder && !conv))
    return Status::Error("ELEMENT_CREATE_FAILED", err);

  // --- depay tuning (E3: configurable per snapshot) ---
  // rtph264depay defaults are FALSE; apply only when config enables them.
  if (input.rtp_wait_for_keyframe)
    g_object_set(depay, "wait-for-keyframe", TRUE, nullptr);
  if (input.rtp_request_keyframe)
    g_object_set(depay, "request-keyframe", TRUE, nullptr);

  ConfigureBoundedQueue(queue, 10);

  // --- rtspsrc properties ---
  // latency=0 is too aggressive on hotspot RTSP (TCP glitches → bus ERROR).
  // 200ms jitter buffer improves multi-cam stability without large delay.
  // timeout=60s: HIKVISION via mediamtx can stall >5s; the old 5s value
  // posted GstRTSPSrcTimeout → EOS and killed the whole WebRTC process.
  g_object_set(src, "location", input.rtsp_url.c_str(),
                    "latency", (gint)200,
                    "drop-on-latency", TRUE,
                    "timeout", (guint64)60000000,
                    nullptr);
  SetFlagsPropertyByNick(src, "protocols", "tcp", &err);

  // --- decoder io-mode (E3: was hardcoded "dmabuf"; now snapshot-driven) ---
  if (IsV4l2HwDecoder(input.decoder_element)) {
    if (!SetEnumPropertyByNick(decoder, "capture-io-mode",
                               input.capture_io_mode.c_str(), &err))
      return Status::Error("DECODER_DMABUF_FAILED", err);
    if (!SetEnumPropertyByNick(decoder, "output-io-mode",
                               input.output_io_mode.c_str(), &err))
      return Status::Error("DECODER_DMABUF_FAILED", err);
  }

  // --- capsfilter ---
  SetCapsFilter(caps, input.decode_output_caps, &err);

  // --- add to pipeline ---
  gst_bin_add_many(GST_BIN(pipeline), src, depay, parse, decoder, caps, queue, nullptr);
  if (conv) gst_bin_add(GST_BIN(pipeline), conv);

  // --- link: 硬解 depay->parse->decoder->caps->queue; 软解插入 videoconvert ---
  std::vector<GstElement*> chain = {depay, parse};
  chain.push_back(decoder);
  if (!is_hw_decoder && conv) chain.push_back(conv);
  chain.push_back(caps);
  chain.push_back(queue);
  if (!LinkElements(chain, &err))
    return Status::Error("LINK_FAILED", err);

  // --- rtspsrc pad-added ---
  auto* ctx = new RtspPadCtx{input.camera_id, depay};
  g_signal_connect(src, "pad-added", G_CALLBACK(OnRtspPadAdded), ctx);

  // Save context for lifetime management
  RuntimeRegistry::RtspPadLinkCtx rctx;
  rctx.camera_id = input.camera_id;
  rctx.depay = depay;
  reg->rtsp_pad_contexts.push_back(rctx);

  // Frame-tagging probe: assign camera_id + frame_id at queue sink
  // (before mux; dropped frames still consume a frame_id).
  {
    GstPad *q_sink = gst_element_get_static_pad(queue, "sink");
    if (q_sink) {
      auto *tag_ctx = g_new0(FrameTagCtx, 1);
      tag_ctx->camera_id = g_strdup(input.camera_id.c_str());
      gst_pad_add_probe(q_sink, GST_PAD_PROBE_TYPE_BUFFER,
                        OnFrameTagProbe, tag_ctx, frame_tag_ctx_free);
      reg->frame_count_ctx_in.push_back(tag_ctx);
      gst_object_unref(q_sink);
    }
  }

  // --- link queue -> mux request pad ---
  Status st = LinkQueueToMux(queue, mux, input);
  if (!st.ok) return st;

  reg->stream_source_by_camera[input.camera_id] = src;
  reg->stream_queue_by_camera[input.camera_id]  = queue;
  reg->camera_id_by_rtsp_url[input.rtsp_url]    = input.camera_id;

  return Status::Ok();
}

// ===================================================================
// LinkQueueToMux
// ===================================================================
Status GstPipelineBuilder::LinkQueueToMux(
    GstElement* queue, GstElement* mux, const StreamInputPlan& input) const {

  GstPad* mux_sink = gst_element_request_pad_simple(mux,
      input.mux_sink_pad_name.c_str());
  if (!mux_sink)
    return Status::Error("MUX_PAD_FAILED",
                         "cannot request " + input.mux_sink_pad_name);

  // pad qdata: camera-id, mux-slot-id
  g_object_set_data_full(G_OBJECT(mux_sink), "camera-id",
                         g_strdup(input.camera_id.c_str()), g_free);
  g_object_set_data(G_OBJECT(mux_sink), "mux-slot-id",
                    GUINT_TO_POINTER(input.mux_slot_id));

  GstPad* queue_src = gst_element_get_static_pad(queue, "src");
  GstPadLinkReturn ret = gst_pad_link(queue_src, mux_sink);
  gst_object_unref(queue_src);
  gst_object_unref(mux_sink);  // mux element owns the pad

  if (ret != GST_PAD_LINK_OK)
    return Status::Error("PAD_LINK_FAILED",
                         "queue -> " + input.mux_sink_pad_name);
  return Status::Ok();
}

// ===================================================================
// BuildRecordingTail — shared composer + encoder + muxer + filesink
//   Called once before the per-group loop when enable_mosaic_recording.
// ===================================================================
Status GstPipelineBuilder::BuildRecordingTail(GstElement* pipeline,
                                               RuntimeRegistry* reg) const {
  std::string err;

  // Grid geometry: mosaic_cols = columns; total_tiles = total cameras.
  // Supports group=4 (4x4), group=2 (8 groups x 2 → still 4-col grid),
  // and group=1 (16 groups x 1 → still 4-col grid).
  const int total_tiles = reg->total_cameras > 0 ? reg->total_cameras
                                                 : (opts_.mosaic_cols * opts_.mosaic_cols);
  const int grid_cols   = opts_.mosaic_cols > 0 ? opts_.mosaic_cols : 4;
  const int grid_rows   = (total_tiles + grid_cols - 1) / grid_cols;
  const int mosaic_w    = opts_.tile_w * grid_cols;
  const int mosaic_h    = opts_.tile_h * grid_rows;

  // Restore force-live construct path that produced ~30s/299f (080718),
  // but without leaky queues (suspected of collapsing mosaic duration).
  GstElement* comp = nullptr;
  {
    GstElementFactory* fac = gst_element_factory_find("qtivcomposer");
    if (fac) {
      gst_plugin_feature_load(GST_PLUGIN_FEATURE(fac));
      GType t = gst_element_factory_get_element_type(fac);
      if (t != G_TYPE_INVALID) {
        comp = GST_ELEMENT(g_object_new(t,
            "name", "mosaic_composer",
            "force-live", TRUE,
            "latency", (guint64)(300 * GST_MSECOND),
            "background", (guint)0,
            nullptr));
      }
      gst_object_unref(fac);
    }
    if (!comp) {
      comp = MakeElement("qtivcomposer", "mosaic_composer", &err);
      fprintf(stderr, "[recording] WARNING: qtivcomposer created without force-live\n");
    }
  }
  if (!comp)
    return Status::Error("RECORDING_TAIL_FAILED", err);

  GstElement* enc  = MakeElement("v4l2h264enc",   "mosaic_encoder",  &err);
  GstElement* parse= MakeElement("h264parse",     "mosaic_h264parse",&err);
  GstElement* mux  = MakeElement("matroskamux",   "mosaic_matroskamux", &err);
  GstElement* fsink= MakeElement("filesink",      "mosaic_filesink", &err);
  if (!comp || !enc || !parse || !mux || !fsink)
    return Status::Error("RECORDING_TAIL_FAILED", err);

  g_object_set(fsink, "location", opts_.mosaic_output_path.c_str(),
                    "sync", FALSE, nullptr);
  g_object_set(enc,   "target-bitrate", (guint)4000000, nullptr);
  // streamable=true: finalize MKV cleanly without waiting for EOS on all pads.
  // In the 16-way demux→compositor→encoder chain, EOS timing across 4 groups
  // and 16 compositor sink pads is not synchronized, causing matroskamux to
  // hang indefinitely waiting for the last pad's EOS. streamable mode tells
  // the muxer this is a live recording and to close the file when EOS arrives
  // on any pad or the pipeline stops.
  g_object_set(mux, "streamable", TRUE, nullptr);

  gst_bin_add_many(GST_BIN(pipeline), comp, enc, parse, mux, fsink, nullptr);

  // caps negotiation between composer and encoder
  GstElement* capsflt = MakeElement("capsfilter", "mosaic_capsflt", &err);
  if (!capsflt) return Status::Error("RECORDING_TAIL_FAILED", "capsfilter: " + err);

  GstCaps* mosaic_caps = gst_caps_from_string(
      ("video/x-raw,format=NV12,"
       "width=" + std::to_string(mosaic_w) + ","
       "height=" + std::to_string(mosaic_h)).c_str());
  g_object_set(capsflt, "caps", mosaic_caps, nullptr);
  gst_caps_unref(mosaic_caps);
  gst_bin_add(GST_BIN(pipeline), capsflt);

  std::vector<GstElement*> tail = {comp, capsflt, enc, parse, mux, fsink};
  if (!LinkElements(tail, &err))
    return Status::Error("RECORDING_TAIL_LINK_FAILED",
                         "comp -> capsflt -> enc -> parse -> mux -> filesink: " + err);

  // Pre-request all compositor sink pads and set tile geometry
  for (int i = 0; i < total_tiles; ++i) {
    GstPad* pad = gst_element_request_pad_simple(comp, "sink_%u");
    if (!pad) {
      fprintf(stderr, "[recording] WARNING: could not pre-request composer sink_%d\n", i);
      continue;
    }
    int col = i % grid_cols;
    int row = i / grid_cols;
    int x   = col * opts_.tile_w;
    int y   = row * opts_.tile_h;

    GValue pos_arr = G_VALUE_INIT;
    g_value_init(&pos_arr, GST_TYPE_ARRAY);
    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_INT);
    g_value_set_int(&v, x); gst_value_array_append_value(&pos_arr, &v);
    g_value_set_int(&v, y); gst_value_array_append_value(&pos_arr, &v);
    g_object_set_property(G_OBJECT(pad), "position", &pos_arr);
    g_value_unset(&pos_arr);
    g_value_unset(&v);

    GValue dim_arr = G_VALUE_INIT;
    g_value_init(&dim_arr, GST_TYPE_ARRAY);
    g_value_init(&v, G_TYPE_INT);
    g_value_set_int(&v, opts_.tile_w);  gst_value_array_append_value(&dim_arr, &v);
    g_value_set_int(&v, opts_.tile_h);  gst_value_array_append_value(&dim_arr, &v);
    g_object_set_property(G_OBJECT(pad), "dimensions", &dim_arr);
    g_value_unset(&dim_arr);
    g_value_unset(&v);

    g_object_set(pad, "max-last-buffer-repeat",
                 (guint64)(24 * 3600 * GST_SECOND), nullptr);

    gst_object_unref(pad);
    fprintf(stderr, "[recording] compositor sink_%d → tile col=%d row=%d at (%d,%d) %dx%d\n",
            i, col, row, x, y, opts_.tile_w, opts_.tile_h);
  }

  reg->recording_compositor = comp;
  reg->recording_encoder    = enc;
  reg->recording_muxer      = mux;
  reg->recording_filesink   = fsink;
  reg->recording_sink_idx   = 0;

  fprintf(stderr, "[recording] mosaic tail built: %dx%d (%d tiles, %d cols x %d rows) → %s\n",
          mosaic_w, mosaic_h, total_tiles, grid_cols, grid_rows,
          opts_.mosaic_output_path.c_str());
  return Status::Ok();
}

// ===================================================================
// BuildGroupRecordingBranch — per-group overlay+demux → compositor slots
//   overlay_src: element whose "src" pad feeds qtivoverlay
//   group_global_slot_base: first compositor sink index for this group (0,4,8,12)
// ===================================================================
// ===================================================================
// BuildGroupRecordingBranch — per-group demux → compositor slots
//   overlay_src: already-overlaid stream (qtivoverlay sits BEFORE tee_terminal
//   so it gets a writable DMA-BUF from metamux; tee then fans out to appsink
//   + this demux branch). Do NOT put qtivoverlay after tee: shared buffers
//   are non-writable and overlay silently skips ("not writable, skipping!").
//   Do NOT gst_buffer_make_writable here: that copies off DMA-BUF and
//   qtivoverlay then fails with "Failed to apply overlays!".
//   group_global_slot_base: first compositor sink index for this group (0,4,8,12)
// ===================================================================
Status GstPipelineBuilder::BuildGroupRecordingBranch(
    GstElement* pipeline,
    GstElement* overlay_src,
    const std::string& group_id,
    int group_global_slot_base,
    const std::vector<std::string>& camera_ids,
    RuntimeRegistry* reg) const {

  std::string err;
  int cams_in_group = static_cast<int>(camera_ids.size());

  if (cams_in_group <= 0)
    return Status::Error("RECORDING_BRANCH_FAILED",
                         group_id + ": camera_ids must not be empty");

  GstElement* demux = MakeElement("aiboxstreamdemux", "demux_" + group_id, &err);
  if (!demux)
    return Status::Error("RECORDING_BRANCH_FAILED", group_id + ": " + err);

  g_object_set(demux,
               "max-srcs", (guint)cams_in_group,
               "silent", TRUE,
               "output-fps", (guint)opts_.mosaic_fps,
               nullptr);

  gst_bin_add(GST_BIN(pipeline), demux);

  if (!gst_element_link(overlay_src, demux))
    return Status::Error("RECORDING_BRANCH_LINK_FAILED",
                         "overlay_src -> demux: " + group_id);

  GstElement* comp = reg->recording_compositor;
  for (int slot = 0; slot < cams_in_group; ++slot) {
    int global_idx = group_global_slot_base + slot;
    const std::string& cam_id = camera_ids[static_cast<size_t>(slot)];

    /* Pad name uses slot index; real camera_id is bound via qdata AFTER
     * request_pad. Demux lazily syncs sp->camera_id on first lookup. */
    gchar demux_src_name[16];
    g_snprintf(demux_src_name, sizeof(demux_src_name), "src_%d", slot);
    GstPad* demux_src = gst_element_request_pad_simple(demux, demux_src_name);
    if (!demux_src)
      return Status::Error("DEMUX_PAD_FAILED",
                           group_id + " slot " + std::to_string(slot));

    g_object_set_data_full(G_OBJECT(demux_src), "camera-id",
                           g_strdup(cam_id.c_str()), g_free);

    // Get pre-allocated compositor sink pad by index
    gchar comp_sink_name[32];
    g_snprintf(comp_sink_name, sizeof(comp_sink_name), "sink_%d", global_idx);
    GstPad* comp_sink = gst_element_get_static_pad(comp, comp_sink_name);
    if (!comp_sink) {
      // Try request if not pre-allocated
      comp_sink = gst_element_request_pad_simple(comp, "sink_%u");
    }
    if (!comp_sink) {
      gst_object_unref(demux_src);
      return Status::Error("COMPOSER_PAD_FAILED",
                           "composer sink for global_idx " + std::to_string(global_idx));
    }

    // Insert queue to decouple demux → composer
    gchar q_name[64];
    g_snprintf(q_name, sizeof(q_name), "q_rec_%s_slot%d", group_id.c_str(), slot);
    GstElement* q = MakeElement("queue", q_name, &err);
    if (!q) { gst_object_unref(demux_src); gst_object_unref(comp_sink); return Status::Error("QUEUE_FAILED", err); }
    // Bounded queue (non-leaky): leaky=downstream was dropping most mosaic
    // frames on IQ9 after the plan changes (MKV stuck at ~1s).
    ConfigureBoundedQueue(q, 4);
    gst_bin_add(GST_BIN(pipeline), q);

    g_object_set(comp_sink, "max-last-buffer-repeat",
                 (guint64)(24 * 3600 * GST_SECOND), nullptr);

    GstPad* q_sink  = gst_element_get_static_pad(q, "sink");
    GstPad* q_src   = gst_element_get_static_pad(q, "src");

    if (gst_pad_link(demux_src, q_sink) != GST_PAD_LINK_OK ||
        gst_pad_link(q_src, comp_sink) != GST_PAD_LINK_OK)
      return Status::Error("DEMUX_TO_COMPOSER_LINK_FAILED",
                           group_id + " slot " + std::to_string(slot) +
                           " → composer sink_" + std::to_string(global_idx));

    gst_object_unref(demux_src);
    gst_object_unref(comp_sink);
    gst_object_unref(q_sink);
    gst_object_unref(q_src);

    fprintf(stderr, "[recording] %s slot%d → composer sink_%d\n",
            group_id.c_str(), slot, global_idx);
  }

  reg->recording_sink_idx += cams_in_group;
  return Status::Ok();
}

// ===================================================================
// Build (main entry point — P1: mux + decode + fakesink only)
// ===================================================================
Result<GstPipelineBuildResult> GstPipelineBuilder::Build(
    const ExecutionPlan& plan) {

  GstPipelineBuildResult result;

  DiagEnableIfRequested();

  if (opts_.validate_element_factories) {
    Status st = ValidateFactories(plan);
    if (!st.ok) return {st, GstPipelineBuildResult{}};
  }

  // --- pipeline ---
  result.pipeline = gst_pipeline_new(
      plan.gst_pipeline_plan.pipeline_name.c_str());
  if (!result.pipeline)
    return {Status::Error("PIPELINE_FAILED", "cannot create GstPipeline"),
            GstPipelineBuildResult{}};

  auto* reg = &result.registry;
  reg->pipeline = result.pipeline;

  const bool use_b2 = ShouldDispatchBatch(plan);
  if (plan.execution_mode == ExecutionMode::kMultistreamBatch &&
      plan.batch_group_plans.empty()) {
    gst_object_unref(result.pipeline);
    return {Status::Error("BATCH_DISPATCH_INCONSISTENT",
                          "execution_mode=multistream_batch but "
                          "batch_group_plans is empty"),
            GstPipelineBuildResult{}};
  }
  if (!use_b2 && !plan.batch_group_plans.empty()) {
    gst_object_unref(result.pipeline);
    return {Status::Error("BATCH_DISPATCH_INCONSISTENT",
                          "batch_group_plans set but execution_mode is "
                          "not multistream_batch"),
            GstPipelineBuildResult{}};
  }

  // Count cameras for VPU budget / registry
  int total_cams = 0;
  if (use_b2) {
    for (const auto& bg : plan.batch_group_plans)
      total_cams += static_cast<int>(bg.streams.size());
  } else {
  for (const auto& tmux : plan.time_mux_group_plans)
    total_cams += static_cast<int>(tmux.stream_inputs.size());
  }
  reg->total_cameras = total_cams > 0 ? total_cams : 16;

  if (use_b2) {
    fprintf(stderr,
            "[BATCH_DISPATCH] path=BuildBatchPipeline groups=%zu cams=%d\n",
            plan.batch_group_plans.size(), total_cams);
    trigger_aibox_frame_meta_registration();
    Status st = BuildBatchPipeline(plan, &result);
    if (!st.ok) {
      DetachBatchQnnProbes(*reg);
      ReleaseAllRequestPads(*reg);
      gst_object_unref(result.pipeline);
      return {st, GstPipelineBuildResult{}};
    }
    return {Status::Ok(), std::move(result)};
  }
  fprintf(stderr, "[BATCH_DISPATCH] path=time_mux groups=%zu\n",
          plan.time_mux_group_plans.size());

  // --- VPU budget validation (WebRTC encoders) ---
  // P3b lazy default: encoders are not created at build (count 0 here).
  // Static enc count only when AIBOX_CAMSPLIT_MAX_ENC is set.
  if (opts_.enable_webrtc || opts_.enable_annotated_rtsp) {
    int dec_count = total_cams;
    int webrtc_enc_count = 0;
    if (opts_.enable_annotated_rtsp) {
      // Continuous mode: one always-on encoder per camera (annotated RTSP).
      webrtc_enc_count = total_cams;
    } else if (const char* me = g_getenv("AIBOX_CAMSPLIT_MAX_ENC")) {
      int v = atoi(me);
      if (v < 0) v = 0;
      webrtc_enc_count = (v > total_cams) ? total_cams : v;
    } else if (g_getenv("AIBOX_CAMSPLIT_FAKESINK") &&
               g_getenv("AIBOX_CAMSPLIT_FAKESINK")[0] == '1') {
      webrtc_enc_count = 0;
    } else {
      // Lazy: no encoders at build
      webrtc_enc_count = 0;
    }
    int rec_enc_count = opts_.enable_mosaic_recording ? 1 : 0;
    int total_vpu = dec_count + webrtc_enc_count + rec_enc_count;
    if (total_vpu > 32) {
      gst_object_unref(result.pipeline);
      return {Status::Error("VPU_BUDGET_EXCEEDED",
              "VPU session budget exceeded: " + std::to_string(dec_count) +
              " dec + " + std::to_string(webrtc_enc_count) +
              " webrtc_enc + " + std::to_string(rec_enc_count) +
              " rec_enc = " + std::to_string(total_vpu) + " > 32"),
              GstPipelineBuildResult{}};
    }
    fprintf(stderr,
            "[webrtc] VPU budget OK: %d dec + %d webrtc_enc(build) + %d rec_enc = %d / 32"
            " (annotated=continuous; otherwise lazy unless MAX_ENC set)\n",
            dec_count, webrtc_enc_count, rec_enc_count, total_vpu);
  }

  // --- Phase 5: build shared mosaic recording tail (compositor + encoder + muxer) ---
  if (opts_.enable_mosaic_recording) {
    Status st = BuildRecordingTail(result.pipeline, reg);
    if (!st.ok) {
      gst_object_unref(result.pipeline);
      return {st, GstPipelineBuildResult{}};
    }
  }

  // --- build mux ---
  int group_recording_slot_base = 0;
  int group_ordinal = 0;
  bool meta_reg_triggered = false;
  for (auto& tmux : plan.time_mux_group_plans) {
    const int this_group = group_ordinal++;
    g_diag_group = this_group;  // per-group diag id isolation
    Status st = BuildMux(tmux, result.pipeline, reg);
    if (!st.ok) {
      gst_object_unref(result.pipeline);
      return {st, GstPipelineBuildResult{}};
    }
    if (!meta_reg_triggered) {
      trigger_aibox_frame_meta_registration();
      meta_reg_triggered = true;
    }

    GstElement* mux = reg->mux_by_group[tmux.time_mux_group_id];

    // --- build decode branches ---
    for (auto& inp : tmux.stream_inputs) {
      st = BuildDecodeBranch(inp, tmux, result.pipeline, mux, reg);
      if (!st.ok) {
        gst_object_unref(result.pipeline);
        return {st, GstPipelineBuildResult{}};
      }
    }

    // --- inference chain (chained qtimetamux, strict serial) or fakesink ---
    if (opts_.enable_yolo_stages && !plan.stage_plans.empty()) {
      std::string err;
      GstElement* mux_q = MakeElement("queue",
          "queue_after_mux_" + tmux.time_mux_group_id, &err);
      ConfigureBoundedQueue(mux_q, 10);
      gst_bin_add(GST_BIN(result.pipeline), mux_q);
      if (!gst_element_link(mux, mux_q))
        return {Status::Error("LINK_FAILED", "mux -> mux_q"), GstPipelineBuildResult{}};

      GstElement* prev_src     = mux_q;
      GstElement* last_metamux = nullptr;

      for (size_t si = 0; si < plan.stage_plans.size(); ++si) {
        const StagePlan& sp = plan.stage_plans[si];
        if (sp.time_mux_group_id != tmux.time_mux_group_id) continue;

        const std::string sfx = sp.stage_id + "_" + tmux.time_mux_group_id;

        StageElements elems;
        Status st = BuildStage(sp, result.pipeline, reg, &elems);
        if (!st.ok) { gst_object_unref(result.pipeline); return {st, GstPipelineBuildResult{}}; }

        GstElement* tee   = MakeElement("tee",         "tee_"      + sfx, &err);
        GstElement* q_inf = MakeElement("queue",       "q_inf_"    + sfx, &err);
        GstElement* q_byp = MakeElement("queue",       "q_byp_"    + sfx, &err);
        GstElement* q_txt = MakeElement("queue",       "q_text_"   + sfx, &err);
        GstElement* mmux  = MakeElement("qtimetamux",  "metamux_"  + sfx, &err);
        if (!tee || !q_inf || !q_byp || !q_txt || !mmux)
          return {Status::Error("STAGE_ELEM_CREATE_FAILED", sfx + ": " + err),
                  GstPipelineBuildResult{}};

        // P1 (3/3 vote, 2026-08-19): non-leaky inf/bypass so qtimetamux(sync)
        // can pair the same frame. Load-shed stays on aiboxtimemux
        // drop-policy=latest. Leaky dual-arm was 1912 QNN vs 231 OUT.
        ConfigureBoundedQueue(q_inf, 10, 0);
        ConfigureBoundedQueue(q_byp, 10, 0);
        ConfigureBoundedQueue(q_txt, 10);

        gst_util_set_object_arg(G_OBJECT(mmux), "mode", "sync");
        g_object_set(mmux, "latency", (guint64)(2 * GST_SECOND), nullptr);
        gst_bin_add_many(GST_BIN(result.pipeline),
                         tee, q_inf, q_byp, q_txt, mmux, nullptr);

        if (!gst_element_link(prev_src, tee))
          return {Status::Error("LINK_FAILED", "prev -> tee " + sfx),
                  GstPipelineBuildResult{}};

        if (!gst_element_link(tee, q_inf))
          return {Status::Error("LINK_FAILED", "tee -> q_inf " + sfx),
                  GstPipelineBuildResult{}};
        if (!gst_element_link(q_inf, elems.first))
          return {Status::Error("LINK_FAILED", "q_inf -> stage " + sfx),
                  GstPipelineBuildResult{}};
        if (!gst_element_link(tee, q_byp) ||
            !gst_element_link(q_byp, mmux))
          return {Status::Error("LINK_FAILED", "tee -> q_byp -> metamux.sink " + sfx),
                  GstPipelineBuildResult{}};
        st = LinkPostOutputToMetamux(elems.post, q_txt, mmux, sfx);
        if (!st.ok) { gst_object_unref(result.pipeline); return {st, GstPipelineBuildResult{}}; }

        prev_src     = mmux;
        last_metamux = mmux;
      }

      if (!last_metamux)
        return {Status::Error("NO_STAGE_FOR_GROUP", tmux.time_mux_group_id),
                GstPipelineBuildResult{}};

      // Terminal topology:
      //   last_metamux → qtivoverlay → tee_post
      //     Branch A: queue → appsink   (events; ROI meta still present)
      //     Branch B: queue → recording demux → compositor (if --record)
      //     Branch C: queue → webrtc demux → per-cam enc → tee_webrtc (if webrtc)
      // Overlay MUST sit before tee: after tee buffers are shared/non-writable.
      // WebRTC mode also requires overlay (annotated frames), NOT gated on --record.

      bool need_overlay =
          opts_.enable_mosaic_recording || opts_.enable_webrtc ||
          opts_.enable_annotated_rtsp ||
          plan.face_plan.enabled;  // face whitening also needs overlay
      GstElement* tee_post = nullptr;
      GstElement* term_src = last_metamux;

      if (need_overlay) {
        GstElement* overlay = MakeElement("qtivoverlay",
            "overlay_" + tmux.time_mux_group_id, &err);
        tee_post = MakeElement("tee",
            "tee_post_" + tmux.time_mux_group_id, &err);
        if (!overlay || !tee_post)
          return {Status::Error("TERMINAL_TEE_FAILED", err),
                  GstPipelineBuildResult{}};
        gst_bin_add_many(GST_BIN(result.pipeline), overlay, tee_post, nullptr);

        // --- Face detection INLINE (same-buffer meta → overlay masks) ---
        // Topology: term_src → q_face_in(max=1,leaky=downstream)
        //           → faceinfer → q_face_out(max=2,non-leaky) → overlay
        if (plan.face_plan.enabled) {
          const std::string grp = tmux.time_mux_group_id;
          // Expert A/B single-variable switch (2026-09-06 round 3):
          //   A (default): last_metamux → q_face_in(max=1,leaky) → faceinfer
          //                → q_face_out → overlay
          //   B (AIBOX_FACE_SKIP_QFACE_IN=1): last_metamux → faceinfer →
          //                q_face_out → overlay  (ONLY q_face_in removed)
          // The pre-infer queue is named "qfi_g<ordinal>" so its src-task
          // thread comm survives the 15-char truncation → unambiguous
          // LWP↔group mapping in gdb/comm dumps.
          const bool skip_qfi =
              (g_getenv("AIBOX_FACE_SKIP_QFACE_IN") != nullptr);
          GstElement* q_face_in = nullptr;
          if (!skip_qfi) {
            q_face_in = MakeElement("queue",
                "qfi_g" + std::to_string(this_group), &err);
          }
          GstElement* faceinfer  = MakeElement(
              plan.face_plan.element.c_str(), "face_infer_" + grp, &err);
          // short name: src-task thread comm survives 15-char truncation
          GstElement* q_face_out = MakeElement("queue",
              "qfo_g" + std::to_string(this_group), &err);
          if ((!skip_qfi && !q_face_in) || !faceinfer || !q_face_out)
            return {Status::Error("FACE_BRANCH_FAILED",
                    "create face elements for " + grp + ": " + err),
                    GstPipelineBuildResult{}};

          if (q_face_in)
            ConfigureBoundedQueue(q_face_in, 1, 2);
          ConfigureBoundedQueue(q_face_out, 2, 0);
          if (!plan.face_plan.config_path.empty())
            g_object_set(faceinfer, "config-path",
                         plan.face_plan.config_path.c_str(), nullptr);
          const int n_htp = DetectHtpCount();
          const int htp_id = (n_htp > 1) ? ((this_group + 1) % 2) : 0;
          g_object_set(faceinfer, "backend-device-id", htp_id, nullptr);
          // pool-size = cameras-in-group + 2 (1 in-flight + 1 copying headroom)
          const guint cam_count =
              static_cast<guint>(tmux.stream_inputs.size());
          const guint pool_size = std::min<guint>(cam_count + 2, 16u);
          g_object_set(faceinfer, "pool-size", pool_size, nullptr);
          fprintf(stderr,
                  "[HTP] face group=%s ordinal=%d -> backend-device-id=%d (n_htp=%d)\n",
                  grp.c_str(), this_group, htp_id, n_htp);
          fprintf(stderr, "[face] group=%s cameras=%u -> pool-size=%u\n",
                  grp.c_str(), cam_count, pool_size);

          if (q_face_in)
            gst_bin_add(GST_BIN(result.pipeline), q_face_in);
          gst_bin_add_many(GST_BIN(result.pipeline),
                           faceinfer, q_face_out, nullptr);
          DiagAttach(term_src, "src", D_LM_SRC);  // term_src == last_metamux
          const bool linked = q_face_in
              ? (gst_element_link(term_src, q_face_in)  &&
                 gst_element_link(q_face_in, faceinfer) &&
                 gst_element_link(faceinfer, q_face_out))
              : (gst_element_link(term_src, faceinfer) &&
                 gst_element_link(faceinfer, q_face_out));
          if (!linked)
            return {Status::Error("FACE_LINK_FAILED",
                    "face branch link failed: " + grp),
                    GstPipelineBuildResult{}};
          if (q_face_in) {
            DiagAttach(q_face_in, "sink", D_QFI_SINK);
            DiagAttach(q_face_in, "src", D_QFI_SRC);
            g_diag_qfi[this_group] = q_face_in;  // [diagq] state sampling
          } else {
            fprintf(stderr,
                    "[face] q_face_in REMOVED for group %s "
                    "(AIBOX_FACE_SKIP_QFACE_IN=1, B mode)\n", grp.c_str());
          }
          DiagAttach(faceinfer, "sink", D_FI_SINK);
          DiagAttach(faceinfer, "src", D_FI_SRC);
          DiagAttach(q_face_out, "sink", D_QFO_SINK);
          DiagAttach(q_face_out, "src", D_QFO_SRC);
          g_diag_qfo[this_group] = q_face_out;  // [diagq] state sampling

          term_src = q_face_out;
          fprintf(stderr, "[face] qtifaceinfer INLINE for group %s\n",
                  grp.c_str());

          GstPad* face_src = gst_element_get_static_pad(faceinfer, "src");
          if (face_src) {
            gst_pad_add_probe(face_src, GST_PAD_PROBE_TYPE_BUFFER,
                OnFaceSyncSampleProbe,
                const_cast<char*>("qtifaceinfer.src"), nullptr);
            gst_object_unref(face_src);
          }
        }  // plan.face_plan.enabled

        {
          std::string probe_err;
          if (!AttachOverlayMaskProbe(overlay, this_group, &probe_err))
            return {Status::Error("FACE_PROBE_FAILED",
                    probe_err + " for " + tmux.time_mux_group_id),
                    GstPipelineBuildResult{}};
        }

        DiagAttach(overlay, "src", D_OV_SRC);
        DiagAttach(tee_post, "sink", D_TEE_POST);

        // Diagnostic-only flow taps (expert round 3, AIBOX_FLOWTAP=1):
        //   T0 = q_face_out -> overlay, T1 = overlay -> tee_post.
        // Pure passthrough elements; never inserted in production runs.
        if (FlowTapEnabled()) {
          GstElement* tap_t0 = MakeElement("aiboxflowtap",
              "tap_t0_" + tmux.time_mux_group_id, &err);
          GstElement* tap_t1 = MakeElement("aiboxflowtap",
              "tap_t1_" + tmux.time_mux_group_id, &err);
          if (!tap_t0 || !tap_t1)
            return {Status::Error("FLOWTAP_FAILED", err),
                    GstPipelineBuildResult{}};
          gst_bin_add_many(GST_BIN(result.pipeline), tap_t0, tap_t1, nullptr);
          if (!gst_element_link(term_src, tap_t0) ||
              !gst_element_link(tap_t0, overlay) ||
              !gst_element_link(overlay, tap_t1) ||
              !gst_element_link(tap_t1, tee_post))
            return {Status::Error("LINK_FAILED",
                                  "metamux -> T0 -> overlay -> T1 -> tee_post " +
                                  tmux.time_mux_group_id),
                    GstPipelineBuildResult{}};
        } else if (aibox::pipeline::FaceOcEnabled()) {
          // POC: stock overlaycomposition between qtivoverlay and tee_post.
          // Draws opaque white face rectangles from GstFaceDetectionMeta.
          GstElement* oc_el = MakeElement("overlaycomposition",
                                          "oc_" + tmux.time_mux_group_id, &err);
          if (!oc_el)
            return {Status::Error("FACE_OC_FAILED", err),
                    GstPipelineBuildResult{}};
          std::string oc_err;
          if (!aibox::pipeline::AttachFaceOcDraw(oc_el, &oc_err))
            return {Status::Error("FACE_OC_ATTACH_FAILED", oc_err),
                    GstPipelineBuildResult{}};
          gst_bin_add(GST_BIN(result.pipeline), oc_el);
          if (!gst_element_link(term_src, overlay) ||
              !gst_element_link(overlay, oc_el) ||
              !gst_element_link(oc_el, tee_post))
            return {Status::Error("LINK_FAILED",
                                  "metamux -> overlay -> overlaycomposition -> "
                                  "tee_post " + tmux.time_mux_group_id),
                    GstPipelineBuildResult{}};
        } else if (!gst_element_link(term_src, overlay) ||
                   !gst_element_link(overlay, tee_post))
          return {Status::Error("LINK_FAILED",
                                "metamux -> overlay -> tee_post " +
                                tmux.time_mux_group_id),
                  GstPipelineBuildResult{}};
        const char* why = opts_.enable_mosaic_recording ?
            (opts_.enable_webrtc ? "recording+webrtc" : "recording") : "webrtc";
        fprintf(stderr, "[%s] overlay_%s before tee_post (writable DMA-BUF path)\n",
                why, tmux.time_mux_group_id.c_str());
        term_src = tee_post;
      } else if (NowebFpsEnabled(opts_)) {
        GstElement* tee_fps = MakeElement("tee",
            "tee_fps_" + tmux.time_mux_group_id, &err);
        if (!tee_fps)
          return {Status::Error("NOWEB_FPS_TEE_FAILED", err),
                  GstPipelineBuildResult{}};
        gst_bin_add(GST_BIN(result.pipeline), tee_fps);
        if (!gst_element_link(term_src, tee_fps))
          return {Status::Error("LINK_FAILED",
                                "metamux -> tee_fps " + tmux.time_mux_group_id),
                  GstPipelineBuildResult{}};
        term_src = tee_fps;
      }

      // Branch A — appsink
      GstElement* q_out   = MakeElement("queue",
          "queue_terminal_" + tmux.time_mux_group_id, &err);
      GstElement* appsink = MakeElement("appsink",
          "appsink_terminal_" + tmux.time_mux_group_id, &err);
      if (!q_out || !appsink)
        return {Status::Error("TERMINAL_CREATE_FAILED", err),
                GstPipelineBuildResult{}};
      ConfigureBoundedQueue(q_out, 10);
      g_object_set(appsink, "sync", FALSE, "max-buffers", (guint)8,
                   "emit-signals", TRUE, nullptr);
      gst_bin_add_many(GST_BIN(result.pipeline), q_out, appsink, nullptr);
      if (!gst_element_link_many(term_src, q_out, appsink, nullptr))
        return {Status::Error("LINK_FAILED", "metamux -> q_out -> appsink"),
                GstPipelineBuildResult{}};

      RuntimeRegistry::AppSinkEntry entry;
      entry.stage_id  = "__terminal__";
      entry.branch_id = "metamux_terminal";
      entry.appsink   = appsink;
      reg->appsinks.push_back(entry);

      if (NowebFpsEnabled(opts_)) {
        Status st = BuildNowebFpsDisplayBranch(
            result.pipeline, term_src, tmux, reg);
        if (!st.ok) {
          gst_object_unref(result.pipeline);
          return {st, GstPipelineBuildResult{}};
        }
      }

      // Branch B — mosaic recording (demux only; overlay already applied)
      if (opts_.enable_mosaic_recording) {
        GstElement* q_overlay = MakeElement("queue",
            "q_overlay_" + tmux.time_mux_group_id, &err);
        if (!q_overlay)
          return {Status::Error("TERMINAL_CREATE_FAILED", "q_overlay: " + err),
                  GstPipelineBuildResult{}};
        // Non-leaky bounded queue: leaky was dropping mosaic frames on IQ9.
        ConfigureBoundedQueue(q_overlay, 4);
        gst_bin_add(GST_BIN(result.pipeline), q_overlay);
        if (!gst_element_link(tee_post, q_overlay))
          return {Status::Error("LINK_FAILED",
                                "tee_terminal -> q_overlay " + tmux.time_mux_group_id),
                  GstPipelineBuildResult{}};

        std::vector<std::string> cam_ids;
        for (const auto& sip : tmux.stream_inputs)
          cam_ids.push_back(sip.camera_id);

        Status st = BuildGroupRecordingBranch(
            result.pipeline, q_overlay,
            tmux.time_mux_group_id,
            group_recording_slot_base,
            cam_ids,
            reg);
        if (!st.ok) {
          gst_object_unref(result.pipeline);
          return {st, GstPipelineBuildResult{}};
        }
        group_recording_slot_base += static_cast<int>(tmux.stream_inputs.size());
      }

      // Branch C — WebRTC annotated preview (per-group demux + per-cam enc)
      // Also feeds the annotated RTSP server (shares the same encode chains).
      if ((opts_.enable_webrtc || opts_.enable_annotated_rtsp) && tee_post) {
        Status st = BuildWebrtcAnnotatedBranch(
            result.pipeline, tee_post, tmux, reg);
        if (!st.ok) {
          gst_object_unref(result.pipeline);
          return {st, GstPipelineBuildResult{}};
        }
      }

    } else {
      // P1: mux -> queue -> fakesink (no YOLO)
      std::string err;
      GstElement* sink = MakeElement("fakesink", "sink_" + tmux.time_mux_group_id, &err);
      g_object_set(sink, "sync", FALSE, nullptr);
      gst_bin_add(GST_BIN(result.pipeline), sink);
      GstElement* mux_out_q = MakeElement("queue",
          "queue_after_mux_" + tmux.time_mux_group_id, &err);
      ConfigureBoundedQueue(mux_out_q, 10);
      gst_bin_add(GST_BIN(result.pipeline), mux_out_q);
      if (!gst_element_link_many(mux, mux_out_q, sink, nullptr)) {
        gst_object_unref(result.pipeline);
        return {Status::Error("LINK_FAILED", "mux -> queue -> fakesink"),
                GstPipelineBuildResult{}};
      }
    }
  }

  return {Status::Ok(), std::move(result)};
}

// ===================================================================
// HTP detection (from rebuild: ResolveHtpDeviceId)
// ===================================================================
int GstPipelineBuilder::DetectHtpCount() {
  static int count = -1;
  if (count >= 0) return count;
  count = 1;
  // Check for second HTP core
  if (g_file_test("/dev/fastrpc-cdsp1", G_FILE_TEST_EXISTS))
    count = 2;
  return count;
}
int GstPipelineBuilder::ResolveHtpDeviceId(int stage_index) {
  // stage_index 从 1 起,按可用 HTP 数取模分配到不同 CDSP。
  // DetectHtpCount()=1 时取模恒为 0,自动回退单核。
  // 示例(双 CDSP): stage_index=1 -> 1%2=1(CDSP1), stage_index=2 -> 2%2=0(CDSP0)
  return stage_index % DetectHtpCount();
}

// ===================================================================
// ConfigQtimlVConverter
// ===================================================================
Status GstPipelineBuilder::ConfigQtimlVConverter(GstElement* conv,
                                                  const StagePlan& stage) const {
  std::string err;
  // Converter engine / letterbox / colour layout come from ModelPreprocessConfig
  // via StagePlan. yolo26 defaults remain gles / centre / reverse; pp-yoloe
  // snapshots override to stretch / regular.
  if (!SetEnumPropertyByNick(conv, "engine", stage.converter_engine.c_str(), &err))
    return Status::Error("CONV_ENGINE_FAILED", err);

  gst_util_set_object_arg(G_OBJECT(conv), "mode", "image-batch-non-cumulative");
  gst_util_set_object_arg(G_OBJECT(conv), "image-disposition",
                          stage.image_disposition.c_str());
  gst_util_set_object_arg(G_OBJECT(conv), "subpixel-layout",
                          stage.subpixel_layout.c_str());

  // mean/sigma are GstValueArray of gdouble and only affect FLOAT tensors
  // (sigma divides, mean subtracts). An empty config string means "leave the
  // vendor default untouched", which keeps every pre-existing model
  // bit-for-bit identical. Any parse / finiteness / readback failure is a
  // hard error: silently building a pipeline with the wrong normalisation
  // would produce plausible-looking but wrong detections.
  auto set_gdouble_array_prop =
      [&](const char* prop, const std::string& csv) -> Status {
    if (csv.empty()) return Status::Ok();

    std::vector<double> vals;
    {
      std::istringstream iss(csv);
      std::string tok;
      while (std::getline(iss, tok, ',')) {
        const char* begin = tok.c_str();
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin || *end != '\0')
          return Status::Error("CONV_ARRAY_PARSE",
                               std::string(prop) + " has a non-numeric entry "
                               "\"" + tok + "\" (config: \"" + csv + "\")");
        if (!std::isfinite(v))
          return Status::Error("CONV_ARRAY_NONFINITE",
                               std::string(prop) + " entry is not finite "
                               "(config: \"" + csv + "\")");
        vals.push_back(v);
      }
    }
    if (vals.empty())
      return Status::Error("CONV_ARRAY_EMPTY",
                           std::string(prop) + " config produced no values");
    if (vals.size() != 3)
      return Status::Error("CONV_ARRAY_SIZE",
                           std::string(prop) + " expects 3 entries (R,G,B), got "
                           + std::to_string(vals.size()));
    // A zero sigma would divide by zero downstream - always a config bug.
    if (std::string(prop) == "sigma") {
      for (double v : vals)
        if (v == 0.0)
          return Status::Error("CONV_ARRAY_ZERO_SIGMA",
                               "sigma must not contain 0.0");
    }

    // NOTE: G_VALUE_INIT is not usable in C++; memset + g_value_init.
    GValue arr;
    memset(&arr, 0, sizeof(arr));
    g_value_init(&arr, GST_TYPE_ARRAY);
    for (double v : vals) {
      GValue item;
      memset(&item, 0, sizeof(item));
      g_value_init(&item, G_TYPE_DOUBLE);
      g_value_set_double(&item, v);
      gst_value_array_append_value(&arr, &item);
      g_value_unset(&item);
    }
    // NOTE: must use the GValue-based setter. g_object_set() is the varargs
    // API and would treat this GValue* as the boxed payload pointer for a
    // GstValueArray property, reading garbage field values (observed as a
    // multi-GB g_malloc failure inside GLib).
    g_object_set_property(G_OBJECT(conv), prop, &arr);
    g_value_unset(&arr);

    // Read back and verify: a silently ignored property would leave the
    // model running with the vendor default normalisation.
    GValue got;
    memset(&got, 0, sizeof(got));
    g_value_init(&got, GST_TYPE_ARRAY);
    g_object_get_property(G_OBJECT(conv), prop, &got);
    bool ok = GST_VALUE_HOLDS_ARRAY(&got)
                  ? (gst_value_array_get_size(&got) ==
                     static_cast<gint>(vals.size()))
                  : false;
    if (ok) {
      for (guint i = 0; i < vals.size(); i++) {
        const GValue* item = gst_value_array_get_value(&got, i);
        if (!item || !G_VALUE_HOLDS_DOUBLE(item) ||
            g_value_get_double(item) != vals[i]) {
          ok = false;
          break;
        }
      }
    }
    g_value_unset(&got);

    if (!ok)
      return Status::Error("CONV_ARRAY_READBACK",
                           std::string(prop) + " readback mismatch after setting "
                           "\"" + csv + "\"");
    fprintf(stderr, "[DBG conv] %s readback OK: %s\n", prop, csv.c_str());
    return Status::Ok();
  };

  Status arr_status = set_gdouble_array_prop("mean", stage.converter_mean);
  if (!arr_status.ok)
    return arr_status;
  return set_gdouble_array_prop("sigma", stage.converter_sigma);
}

// ===================================================================
// ConfigQtimlSnpe
// ===================================================================
Status GstPipelineBuilder::ConfigQtimlSnpe(GstElement* snpe,
                                            const StagePlan& stage) const {
  std::string err;
  // Only properties confirmed on board via gst-inspect-1.0 qtimlsnpe:
  // model / delegate / layers / tensors / performance-profile / priority /
  // profiling-level. Never set the qtimlqnn-only backend/system/
  // backend-device-id on this element.
  if (!SetEnumPropertyByNick(snpe, "delegate", "dsp", &err))
    return Status::Error("SNPE_DELEGATE_FAILED", err);

  g_object_set(G_OBJECT(snpe),
      "model", stage.model_path.c_str(),
      nullptr);

  fprintf(stderr, "[SNPE] stage=%s model=%s delegate=dsp\n",
          stage.stage_id.c_str(), stage.model_path.c_str());
  return Status::Ok();
}

// ===================================================================
// ConfigQtimlQnn
// ===================================================================
Status GstPipelineBuilder::ConfigQtimlQnn(GstElement* qnn,
                                           const StagePlan& stage) const {
  int htp_id = ResolveHtpDeviceId(stage.stage_index);
  fprintf(stderr, "[HTP] stage=%s stage_index=%d -> backend-device-id=%d (n_htp=%d)\n",
          stage.stage_id.c_str(), stage.stage_index, htp_id, DetectHtpCount());

  g_object_set(G_OBJECT(qnn),
      "model",  stage.model_path.c_str(),
      "backend", "/usr/lib/libQnnHtp.so",
      "system",  "/usr/lib/libQnnSystem.so",
      "backend-device-id", htp_id,
      nullptr);

  // The system qtimlqnn plug-in does not expose this optional property.  The
  // environment switch therefore keeps the baseline byte-for-byte unchanged,
  // while patched candidates must prove the requested enum was applied.
  const gchar* requested_htp_mode = g_getenv("AIBOX_QNN_HTP_PERFORMANCE_MODE");
  if (requested_htp_mode && requested_htp_mode[0] != '\0') {
    std::string err;
    if (!SetEnumPropertyByNick(qnn, "htp-performance-mode",
                               requested_htp_mode, &err)) {
      return Status::Error("QNN_HTP_PERFORMANCE_MODE_FAILED", err);
    }

    gint applied_mode = -1;
    g_object_get(qnn, "htp-performance-mode", &applied_mode, nullptr);
    GParamSpec* pspec = g_object_class_find_property(
        G_OBJECT_GET_CLASS(qnn), "htp-performance-mode");
    GEnumClass* enum_class = pspec && G_IS_PARAM_SPEC_ENUM(pspec)
        ? G_ENUM_CLASS(g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(pspec)))
        : nullptr;
    const GEnumValue* applied = enum_class
        ? g_enum_get_value(enum_class, applied_mode) : nullptr;
    const bool readback_ok = applied && applied->value_nick &&
        std::strcmp(applied->value_nick, requested_htp_mode) == 0;
    if (enum_class) g_type_class_unref(enum_class);
    if (!readback_ok) {
      return Status::Error("QNN_HTP_PERFORMANCE_MODE_READBACK_FAILED",
          std::string("requested=") + requested_htp_mode);
    }
    fprintf(stderr, "[QNN HTP] stage=%s performance-mode=%s readback=OK\n",
            stage.stage_id.c_str(), requested_htp_mode);
  }

  // Official qtimlqnn has a graphindices bug: setting "tensors" when the
  // model has multiple outputs indexes inputTensors with OUTPUT-derived
  // indices and can SIGSEGV. B2 [output.N] therefore leaves the property
  // unset so all graph outputs flow through. Legacy single [output] still
  // sets name (B1 / YOLO26).
  if (stage.model_output.tensors.size() > 1) {
    fprintf(stderr,
            "[QNN tensors] stage=%s: property NOT set (%zu outputs; "
            "qtimlqnn graphindices workaround)\n",
            stage.stage_id.c_str(), stage.model_output.tensors.size());
  } else {
  gst_util_set_object_arg(G_OBJECT(qnn), "tensors",
                          stage.model_output.name.c_str());
  }

  return Status::Ok();
}

// ===================================================================
// ConfigQtimlPostProcess
// ===================================================================
Status GstPipelineBuilder::ConfigQtimlPostProcess(GstElement* post,
                                                    const StagePlan& stage) const {
  g_object_set(post,
      "labels",   stage.labels_path.c_str(),
      "results",  stage.model_postprocess.results,
      "person-only-boxes", stage.model_postprocess.person_only_boxes ? TRUE : FALSE,
      "hide-person-label", stage.model_postprocess.hide_person_label ? TRUE : FALSE,
      nullptr);

  // class-filter: join StagePlan.class_filter (e.g. ["Green"]) into a
  // comma-separated whitelist string for the plugin's "class-filter"
  // property. Empty vector => property left unset => no filtering.
  if (!stage.class_filter.empty()) {
    std::string joined;
    for (size_t i = 0; i < stage.class_filter.size(); ++i) {
      if (i > 0) joined += ",";
      joined += stage.class_filter[i];
    }
    g_object_set(post, "class-filter", joined.c_str(), nullptr);
    fprintf(stderr, "[DBG class-filter] stage=%s class-filter=\"%s\"\n",
            stage.stage_id.c_str(), joined.c_str());
  }

  // Set module property. gst_util_set_object_arg fails for custom GEnum.
  // Use GType-based lookup with the registered type name.
  {
    gint val = 0;
    GType type = g_type_from_name("GstMLPostProcessModules");
    if (!type) type = g_type_from_name("GstMLModules");
    fprintf(stderr, "[DBG module] stage=%s nick=%s gtype=%lu\n",
            stage.stage_id.c_str(), stage.postprocess_module.c_str(),
            (unsigned long)type);
    if (type) {
      GEnumClass *ec = static_cast<GEnumClass*>(g_type_class_ref(type));
      if (ec) {
        fprintf(stderr, "[DBG module] enum_class=%p n_values=%u\n",
                (void*)ec, ec->n_values);
        for (guint i = 0; i < ec->n_values; i++) {
          fprintf(stderr, "[DBG module]   nick[%u]=%s value=%d\n",
                  i, ec->values[i].value_nick, ec->values[i].value);
        }
        GEnumValue *ev = g_enum_get_value_by_nick(
            ec, stage.postprocess_module.c_str());
        if (ev) {
          val = static_cast<gint>(ev->value);
          g_object_set(post, "module", val, nullptr);
        }
        g_type_class_unref(ec);
      }
    }
    // Verify
    guint check = 0;
    g_object_get(post, "module", &check, nullptr);
    fprintf(stderr, "[DBG module] final check=%u\n", check);
  }

  // settings 必须是 JSON, 且置信度为百分制(0-100)。QTI 后处理模块 Configure()
  // 解析该 JSON, 格式错误会导致 "Failed to set module options!"。
  // 参考 gst-ai-parallel-inference 样例: {"confidence": 40.0}
  double conf = stage.model_postprocess.confidence_thresh;
  if (conf <= 1.0) conf *= 100.0;  // 0-1 小数 -> 百分制
  char settings[64];
  std::snprintf(settings, sizeof(settings), "{\"confidence\": %.1f}", conf);
  gst_util_set_object_arg(G_OBJECT(post), "settings", settings);

  return Status::Ok();
}

// ===================================================================
// BuildStage
// ===================================================================
Status GstPipelineBuilder::BuildStage(const StagePlan& stage,
                                       GstElement* pipeline,
                                       RuntimeRegistry* reg,
                                       StageElements* out) const {
  std::string err;

  GstElement* conv = MakeElement(stage.converter_factory, stage.converter_name, &err);
  GstElement* qnn  = MakeElement(stage.inference_factory, stage.inference_name, &err);
  GstElement* post = MakeElement(stage.postprocess_factory, stage.postprocess_name, &err);
  if (!conv || !qnn || !post)
    return Status::Error("STAGE_ELEMENT_FAILED", stage.stage_id + ": " + err);

  Status st = ConfigQtimlVConverter(conv, stage);
  if (!st.ok) return st;
  if (stage.inference_factory == "qtimlqnn") {
    st = ConfigQtimlQnn(qnn, stage);
  } else if (stage.inference_factory == "qtimlsnpe") {
    st = ConfigQtimlSnpe(qnn, stage);
  } else {
    return Status::Error("UNKNOWN_INFERENCE_FACTORY",
                         stage.stage_id + ": " + stage.inference_factory);
  }
  if (!st.ok) return st;
  st = ConfigQtimlPostProcess(post, stage);
  if (!st.ok) return st;

  gst_bin_add_many(GST_BIN(pipeline), conv, qnn, post, nullptr);
  if (!gst_element_link_many(conv, qnn, post, nullptr))
    return Status::Error("STAGE_LINK_FAILED", stage.stage_id);

  AttachStageLatProbes(conv, stage.converter_name, reg);
  AttachStageLatProbes(qnn, stage.inference_name, reg);
  AttachStageLatProbes(post, stage.postprocess_name, reg);

  out->first = conv;
  out->last  = post;
  out->post  = post;
  return Status::Ok();
}

// ===================================================================
// LinkPostOutputToMetamux — post.src -> queue -> qtimetamux.data_0
// ===================================================================
Status GstPipelineBuilder::LinkPostOutputToMetamux(GstElement* post,
                                              GstElement* q_txt,
                                              GstElement* metamux,
                                              const std::string& tag) const {
  if (!post || !q_txt || !metamux)
    return Status::Error("LINK_TEXT_METAMUX_NULL", tag);

  GstPad* post_src = gst_element_get_static_pad(post, "src");
  GstPad* q_sink = gst_element_get_static_pad(q_txt, "sink");
  GstPad* q_src  = gst_element_get_static_pad(q_txt, "src");
  GstPad* data0  = gst_element_request_pad_simple(metamux, "data_0");

  Status st = Status::Ok();
  if (!post_src || !q_sink || !q_src || !data0) {
    st = Status::Error("METAMUX_PAD_FAILED", tag);
  } else if (gst_pad_link(post_src, q_sink) != GST_PAD_LINK_OK) {
    st = Status::Error("POST_OUTPUT_LINK_FAILED", tag);
  } else if (gst_pad_link(q_src, data0) != GST_PAD_LINK_OK) {
    st = Status::Error("METAMUX_DATA0_LINK_FAILED", tag);
  }

  if (post_src) gst_object_unref(post_src);
  if (q_sink)   gst_object_unref(q_sink);
  if (q_src)    gst_object_unref(q_src);
  if (data0)    gst_object_unref(data0);  // metamux element owns the pad
  return st;
}

// ===================================================================
// BuildWebrtcAnnotatedBranch
//   tee_post → q_demux → [aiboxstreamdemux|aiboxcamsplit] → per-cam(...)
//   Env:
//     AIBOX_WEBRTC_SPLITTER=demux|camsplit  (default: demux, zero-copy)
//     AIBOX_CAPS_PROBE=1        Stage0: skip split/enc; probe caps then fakesink
//     AIBOX_CAMSPLIT_FAKESINK=1 Stage1: splitter → leaky queue → fakesink (no enc)
//     AIBOX_CAMSPLIT_MAX_ENC=N  Stage2 diag: only first N cams get enc; rest fakesink
// ===================================================================
static GstPadProbeReturn
OnWebrtcCapsProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  (void)pad;
  (void)user_data;
  if (!(info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM))
    return GST_PAD_PROBE_OK;
  GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
  if (!ev || GST_EVENT_TYPE(ev) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  GstCaps* caps = nullptr;
  gst_event_parse_caps(ev, &caps);
  gchar* cs = caps ? gst_caps_to_string(caps) : nullptr;
  fprintf(stderr, "[CAPS_PROBE] tee_post/q_demux caps=%s\n", cs ? cs : "(null)");
  g_free(cs);
  if (caps) {
    guint n = gst_caps_get_size(caps);
    for (guint i = 0; i < n; ++i) {
      GstCapsFeatures* feat = gst_caps_get_features(caps, i);
      gchar* fs = feat ? gst_caps_features_to_string(feat) : nullptr;
      fprintf(stderr, "[CAPS_PROBE] structure[%u] features=%s\n",
              i, fs ? fs : "(none)");
      g_free(fs);
    }
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
OnWebrtcBufFlagProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  (void)pad;
  auto* n = static_cast<guint*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  (*n)++;
  if (*n <= 3 || (*n % 30) == 0) {
    const guint nmem = gst_buffer_n_memory(buf);
    const char* mem_tn = "(none)";
    if (nmem > 0) {
      GstMemory* mem = gst_buffer_peek_memory(buf, 0);
      if (mem) mem_tn = g_type_name(G_TYPE_FROM_INSTANCE(mem));
    }
    fprintf(stderr,
            "[CAPS_PROBE] buf n=%u flags=0x%x pts=%" GST_TIME_FORMAT
            " nmem=%u mem0=%s\n",
            *n, (unsigned)GST_BUFFER_FLAGS(buf),
            GST_TIME_ARGS(GST_BUFFER_PTS(buf)), nmem, mem_tn);
  }
  return GST_PAD_PROBE_OK;
}

Status GstPipelineBuilder::BuildWebrtcAnnotatedBranch(
    GstElement* pipeline,
    GstElement* tee_post,
    const TimeMuxGroupPlan& tmux,
    RuntimeRegistry* reg) const {

  std::string err;
  const auto& group_id = tmux.time_mux_group_id;
  int cams_in_group = static_cast<int>(tmux.stream_inputs.size());

  if (cams_in_group <= 0)
    return Status::Error("WEBRTC_BRANCH_FAILED",
                         group_id + ": cams_in_group must be > 0");

  const bool caps_probe =
      (g_getenv("AIBOX_CAPS_PROBE") && g_getenv("AIBOX_CAPS_PROBE")[0] == '1');
  const bool fakesink_only =
      (g_getenv("AIBOX_CAMSPLIT_FAKESINK") &&
       g_getenv("AIBOX_CAMSPLIT_FAKESINK")[0] == '1');
  int max_enc = 0;  // P3b default: lazy — no encoders at build
  if (const char* me = g_getenv("AIBOX_CAMSPLIT_MAX_ENC")) {
    int v = atoi(me);
    if (v >= 0) max_enc = v;
  }
  if (fakesink_only) max_enc = 0;
  // Annotated RTSP mode: the RTSP server consumes tee_webrtc without any
  // WebRTC viewer session, so every camera in the group needs a persistent
  // static encode chain. The diagnostic fakesink_only switch still wins.
  if (opts_.enable_annotated_rtsp && !fakesink_only)
    max_enc = cams_in_group;

  // Queue between tee_post and splitter
  GstElement* q_demux = MakeElement("queue", "q_demux_webrtc_" + group_id, &err);
  if (!q_demux) return Status::Error("WEBRTC_QUEUE_FAILED", err);
  ConfigureBoundedQueue(q_demux, 4, 2);
  gst_bin_add(GST_BIN(pipeline), q_demux);

  // Diagnostic-only flow tap T2 = tee_post -> q_demux (AIBOX_FLOWTAP=1).
  GstElement* tap_t2 = nullptr;
  if (FlowTapEnabled()) {
    tap_t2 = MakeElement("aiboxflowtap", "tap_t2_" + group_id, &err);
    if (!tap_t2) return Status::Error("WEBRTC_TAP_FAILED", err);
    gst_bin_add(GST_BIN(pipeline), tap_t2);
  }

  GstPad* tee_src = gst_element_request_pad_simple(tee_post, "src_%u");
  if (!tee_src) return Status::Error("WEBRTC_TEE_PAD_FAILED", group_id);
  GstPad* q_sink = gst_element_get_static_pad(q_demux, "sink");
  GstPadLinkReturn lr;
  if (tap_t2) {
    GstPad* t_sink = gst_element_get_static_pad(tap_t2, "sink");
    GstPad* t_src  = gst_element_get_static_pad(tap_t2, "src");
    lr = gst_pad_link(tee_src, t_sink);
    if (lr == GST_PAD_LINK_OK)
      lr = gst_pad_link(t_src, q_sink);
    gst_object_unref(t_sink);
    gst_object_unref(t_src);
  } else {
    lr = gst_pad_link(tee_src, q_sink);
  }
  gst_object_unref(q_sink);
  gst_object_unref(tee_src);
  if (lr != GST_PAD_LINK_OK)
    return Status::Error("WEBRTC_LINK_FAILED", "tee_post -> q_demux " + group_id);

  // ---- Stage0: caps probe only (no splitter / no enc) ----
  if (caps_probe) {
    GstElement* fs = MakeElement("fakesink", "caps_probe_fs_" + group_id, &err);
    if (!fs) return Status::Error("WEBRTC_CAPS_PROBE_FAILED", err);
    g_object_set(fs, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add(GST_BIN(pipeline), fs);
    if (!gst_element_link(q_demux, fs))
      return Status::Error("WEBRTC_LINK_FAILED", "q_demux -> caps_probe_fs");

    GstPad* qs = gst_element_get_static_pad(q_demux, "sink");
    if (qs) {
      gst_pad_add_probe(qs, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                        OnWebrtcCapsProbe, nullptr, nullptr);
      static guint buf_n = 0;
      gst_pad_add_probe(qs, GST_PAD_PROBE_TYPE_BUFFER,
                        OnWebrtcBufFlagProbe, &buf_n, nullptr);
      gst_object_unref(qs);
    }
    fprintf(stderr,
            "[DIAG Stage0] AIBOX_CAPS_PROBE=1: tee_post→q→fakesink (no splitter)\n");
    return Status::Ok();
  }

  // 1. Create splitter: default = aiboxstreamdemux (zero-copy async).
  //    Set AIBOX_WEBRTC_SPLITTER=camsplit to restore appsink→appsrc deep-copy.
  const char* splitter_env = g_getenv("AIBOX_WEBRTC_SPLITTER");
  const bool use_camsplit =
      (splitter_env && g_strcmp0(splitter_env, "camsplit") == 0);
  const char* splitter_factory =
      use_camsplit ? "aiboxcamsplit" : "aiboxstreamdemux";
  const std::string splitter_name =
      std::string(use_camsplit ? "camsplit_webrtc_" : "demux_webrtc_") +
      group_id;

  GstElement* demux = MakeElement(splitter_factory, splitter_name, &err);
  if (!demux)
    return Status::Error("WEBRTC_BRANCH_FAILED", group_id + ": " + err);

  g_object_set(demux,
               "max-srcs", (guint)cams_in_group,
               "silent", FALSE,          // log first-frame caps / route
               "output-fps", (guint)0,   // pass-through PTS; keep zero-copy
               nullptr);
  gst_bin_add(GST_BIN(pipeline), demux);

  // Diagnostic-only flow tap T3 = q_demux -> splitter (AIBOX_FLOWTAP=1).
  if (FlowTapEnabled()) {
    GstElement* tap_t3 = MakeElement("aiboxflowtap", "tap_t3_" + group_id, &err);
    if (!tap_t3) return Status::Error("WEBRTC_TAP_FAILED", err);
    gst_bin_add(GST_BIN(pipeline), tap_t3);
    if (!gst_element_link(q_demux, tap_t3) ||
        !gst_element_link(tap_t3, demux))
      return Status::Error("WEBRTC_LINK_FAILED",
                           std::string("q_demux -> T3 -> ") + splitter_factory +
                           " " + group_id);
  } else if (!gst_element_link(q_demux, demux))
    return Status::Error("WEBRTC_LINK_FAILED",
                         std::string("q_demux -> ") + splitter_factory + " " +
                             group_id);

  DiagAttach(q_demux, "sink", D_QDM_SINK);
  DiagAttach(q_demux, "src", D_QDM_SRC);

  // 2. Per-camera branches
  for (int slot = 0; slot < cams_in_group; ++slot) {
    const auto& input = tmux.stream_inputs[static_cast<size_t>(slot)];
    const auto& cam   = input.camera_id;

    std::string slot_key = group_id + ":" + std::to_string(slot);
    reg->webrtc_slot_to_camera[slot_key] = cam;

    gchar demux_src_name[16];
    g_snprintf(demux_src_name, sizeof(demux_src_name), "src_%d", slot);
    GstPad* demux_src = gst_element_request_pad_simple(demux, demux_src_name);
    if (!demux_src)
      return Status::Error("WEBRTC_SPLITTER_PAD_FAILED",
                           group_id + " slot " + std::to_string(slot));

    g_object_set_data_full(G_OBJECT(demux_src), "camera-id",
                           g_strdup(cam.c_str()), g_free);

    DiagAttachPad(demux_src, D_DM_SRC_BASE + slot);

    // splitter → tee_perm(allow-not-linked) → q_ka → fakesink_ka (keep-alive)
    // EnsureWebrtcEncoder adds enc branch on tee_perm; trunk is never unlinked.
    if (fakesink_only || slot >= max_enc) {
      GstElement* tee_perm = MakeElement("tee", "tee_stub_webrtc_" + cam, &err);
      GstElement* q = MakeElement("queue", "q_ka_webrtc_" + cam, &err);
      GstElement* fsink = MakeElement("fakesink", "fakesink_ka_webrtc_" + cam, &err);
      if (!tee_perm || !q || !fsink) {
        gst_object_unref(demux_src);
        return Status::Error("WEBRTC_ELEM_FAILED", cam + ": " + err);
      }
      g_object_set(tee_perm, "allow-not-linked", TRUE, nullptr);
      ConfigureBoundedQueue(q, 4, 2);
      g_object_set(fsink, "sync", FALSE, "async", FALSE, nullptr);
      gst_bin_add_many(GST_BIN(pipeline), tee_perm, q, fsink, nullptr);

      GstPad* tee_sink = gst_element_get_static_pad(tee_perm, "sink");
      if (!tee_sink || gst_pad_link(demux_src, tee_sink) != GST_PAD_LINK_OK) {
        if (tee_sink) gst_object_unref(tee_sink);
        gst_object_unref(demux_src);
        return Status::Error("WEBRTC_LINK_FAILED", "splitter→tee_perm " + cam);
      }
      gst_object_unref(tee_sink);

      GstPad* tee_src0 = gst_element_request_pad_simple(tee_perm, "src_%u");
      GstPad* qs = gst_element_get_static_pad(q, "sink");
      if (!tee_src0 || !qs || gst_pad_link(tee_src0, qs) != GST_PAD_LINK_OK) {
        if (tee_src0) {
          gst_element_release_request_pad(tee_perm, tee_src0);
          gst_object_unref(tee_src0);
        }
        if (qs) gst_object_unref(qs);
        gst_object_unref(demux_src);
        return Status::Error("WEBRTC_LINK_FAILED", "tee_perm→q " + cam);
      }
      gst_object_unref(tee_src0);
      gst_object_unref(qs);

      if (!gst_element_link(q, fsink)) {
        gst_object_unref(demux_src);
        return Status::Error("WEBRTC_LINK_FAILED", "q→fakesink " + cam);
      }

      // Keep splitter src + tee_perm + keep-alive elems for P3b EnsureWebrtcEncoder
      reg->webrtc_splitter_src_by_camera[cam] = demux_src;  // takes ownership of ref
      reg->webrtc_stub_tee_by_camera[cam] = tee_perm;
      reg->webrtc_stub_queue_by_camera[cam] = q;
      reg->webrtc_stub_fakesink_by_camera[cam] = fsink;

      fprintf(stderr,
              "[webrtc lazy] %s slot%d(%s): %s→tee_perm→q→fakesink stub "
              "(fakesink_only=%d max_enc=%d)\n",
              group_id.c_str(), slot, cam.c_str(),
              use_camsplit ? "camsplit" : "demux",
              (int)fakesink_only, max_enc);
      continue;
    }

    // Full chain: queue → videoscale → capsfilter → enc → parse → tee → fakesink
    GstElement* q      = MakeElement("queue",        "q_webrtc_"      + cam, &err);
    GstElement* vscale = MakeElement("videoscale",   "vscale_webrtc_" + cam, &err);
    GstElement* capsflt= MakeElement("capsfilter",   "caps_webrtc_"   + cam, &err);
    GstElement* enc    = MakeElement("v4l2h264enc",  "enc_webrtc_"    + cam, &err);
    GstElement* parse  = MakeElement("h264parse",    "parse_webrtc_"  + cam, &err);
    GstElement* tee    = MakeElement("tee",          "tee_webrtc_"    + cam, &err);
    GstElement* fsink  = MakeElement("fakesink",     "fakesink_webrtc_" + cam, &err);

    if (!q || !vscale || !capsflt || !enc || !parse || !tee || !fsink) {
      if (demux_src) gst_object_unref(demux_src);
      return Status::Error("WEBRTC_ELEM_FAILED", cam + ": " + err);
    }

    g_object_set(tee, "allow-not-linked", TRUE, nullptr);
    /* P0-1: static path must match the dynamic path semantics — bounded
     * LEAKY pre-encoder queue (one blocked encoder must never back-pressure
     * the demux / main inference chain) and a keepalive fakesink that never
     * prerolls (sync=false + async=false). */
    ConfigureBoundedQueue(q, 4, 2);

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "NV12", nullptr);
    g_object_set(capsflt, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(parse, "config-interval", (gint)-1, nullptr);
    g_object_set(fsink, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), q, vscale, capsflt, enc, parse, tee, fsink,
                     nullptr);

    // Expert round C metrics: encoder chain in/out counts (per cam slot).
    DiagAttach(enc, "sink", D_ENC_SINK_BASE + slot);
    DiagAttach(enc, "src", D_ENC_SRC_BASE + slot);

    // Frame Count Summary OUT: count real per-cam encoder output in the
    // WebRTC path (the noweb path attaches its own out probes elsewhere).
    // Without this the summary printed frames_out=0 for every camera.
    {
      auto* out_ctx = new FrameCountOutCtx{cam, 0};
      GstPad* enc_src = gst_element_get_static_pad(enc, "src");
      if (enc_src) {
        gst_pad_add_probe(enc_src, GST_PAD_PROBE_TYPE_BUFFER,
                          OnFrameOutProbe, out_ctx, nullptr);
        gst_object_unref(enc_src);
      }
      reg->frame_count_ctx_out.push_back(out_ctx);
    }

    if (!gst_element_link_many(q, vscale, capsflt, enc, parse, tee, nullptr)) {
      gst_object_unref(demux_src);
      return Status::Error("WEBRTC_LINK_FAILED", "per-cam chain " + cam);
    }

    GstPad* tee_src0 = gst_element_request_pad_simple(tee, "src_%u");
    GstPad* fsink_pad = gst_element_get_static_pad(fsink, "sink");
    if (!tee_src0 || !fsink_pad ||
        gst_pad_link(tee_src0, fsink_pad) != GST_PAD_LINK_OK) {
      if (tee_src0) { gst_element_release_request_pad(tee, tee_src0); gst_object_unref(tee_src0); }
      if (fsink_pad) gst_object_unref(fsink_pad);
      gst_object_unref(demux_src);
      return Status::Error("WEBRTC_LINK_FAILED", "tee→fakesink " + cam);
    }
    gst_object_unref(tee_src0);
    gst_object_unref(fsink_pad);

    GstPad* cam_q_sink = gst_element_get_static_pad(q, "sink");
    if (!cam_q_sink || gst_pad_link(demux_src, cam_q_sink) != GST_PAD_LINK_OK) {
      if (cam_q_sink) gst_object_unref(cam_q_sink);
      gst_object_unref(demux_src);
      return Status::Error("WEBRTC_LINK_FAILED", "splitter→queue " + cam);
    }
    gst_object_unref(cam_q_sink);
    gst_object_unref(demux_src);

    reg->webrtc_tee_by_camera[cam]         = tee;
    reg->webrtc_encoder_by_camera[cam]     = enc;
    reg->webrtc_h264parse_by_camera[cam]   = parse;
    reg->webrtc_capsfilter_by_camera[cam]  = capsflt;
    reg->webrtc_videoscale_by_camera[cam]  = vscale;
    // Builder-static chain: SessionManager::Shutdown must NOT tear this down
    // (it is owned by the pipeline and released at pipeline NULL); only
    // session-dynamic encoder chains are SessionManager-owned.
    reg->builder_static_webrtc_cams[cam]   = true;

    fprintf(stderr, "[webrtc] %s slot%d(%s): %s→vscale→caps→enc→parse→tee_webrtc\n",
            group_id.c_str(), slot, cam.c_str(),
            use_camsplit ? "camsplit" : "demux");
  }

  return Status::Ok();
}

// ===================================================================
// Batch N — decode → tee → {qtibatch.sink_N, bypass metamux}
//   qtibatch.src → leaky queue → conv → qnn → qtimldemux.src_N → post
//   no aiboxbatchsplit (S2 cascade still uses split, batch=2 only)
// ===================================================================
Status GstPipelineBuilder::BuildBatchPipeline(const ExecutionPlan& plan,
                                              GstPipelineBuildResult* result) const {
  GstElement* pipeline = result->pipeline;
  auto* reg = &result->registry;

  if (opts_.enable_webrtc)
    return Status::Error("PHASE3_NO_WEBRTC",
                         "Phase 3 is noweb only; WebRTC is Phase 5");

  for (const auto& group : plan.batch_group_plans) {
    if (group.batch_size == 0 ||
        group.streams.size() != static_cast<size_t>(group.batch_size))
      return Status::Error("BATCH_GROUP_SIZE_MISMATCH",
                           group.batch_group_id + " streams=" +
                           std::to_string(group.streams.size()) +
                           " batch_size=" + std::to_string(group.batch_size));

    const InferenceChainPlan* chain = nullptr;
    for (const auto& ch : plan.inference_chain_plans)
      if (ch.batch_group_id == group.batch_group_id) { chain = &ch; break; }
    if (!chain)
      return Status::Error("BATCH_CHAIN_MISSING", group.batch_group_id);
    if (chain->stage_ids.size() != 1 && chain->stage_ids.size() != 2)
      return Status::Error("PHASE4_STAGE_COUNT",
                           group.batch_group_id + " has " +
                           std::to_string(chain->stage_ids.size()) +
                           " stages; Phase 4 allows 1 or 2");
    if (chain->stage_ids.size() == 2 && group.batch_size != 2)
      return Status::Error("PHASE4_CASCADE_B2_ONLY",
                           group.batch_group_id +
                           " cascade is batch=2 only");

    for (const auto& bs : group.streams) {
      if (reg->decode_src_by_camera.count(bs.camera_id)) continue;
      const StreamPlan* sp = nullptr;
      for (const auto& s : plan.stream_plans)
        if (s.camera_id == bs.camera_id) { sp = &s; break; }
      if (!sp)
        return Status::Error("BATCH_STREAM_PLAN_MISSING", bs.camera_id);
      const CapsString ingress = !group.input_caps.empty()
          ? group.input_caps : sp->decode.output_caps;
      Status st = BuildBatchDecodeStream(*sp, ingress, pipeline, reg,
                                         group.batch_group_id, bs.stream_id);
      if (!st.ok) return st;
    }

    std::unordered_map<CameraId, GstElement*> video_inputs;
    for (const auto& bs : group.streams) {
      auto qit = reg->stream_queue_by_camera.find(bs.camera_id);
      if (qit == reg->stream_queue_by_camera.end() || !qit->second)
        return Status::Error("BATCH_DECODE_QUEUE_MISSING", bs.camera_id);
      video_inputs[bs.camera_id] = qit->second;
    }

    const StagePlan* stage = nullptr;
    for (const auto& sp : plan.stage_plans)
      if (sp.batch_group_id == group.batch_group_id &&
          sp.stage_id == chain->stage_ids[0]) { stage = &sp; break; }
    if (!stage)
      return Status::Error("BATCH_STAGE_MISSING",
                           group.batch_group_id + ":" + chain->stage_ids[0]);

    std::unordered_map<CameraId, GstElement*> stage_outputs;
    Status st = BuildBatchStage(*stage, group, video_inputs, pipeline, reg,
                                &stage_outputs);
    if (!st.ok) return st;

    if (chain->stage_ids.size() == 2) {
      const StagePlan* stage2 = nullptr;
      for (const auto& sp : plan.stage_plans)
        if (sp.batch_group_id == group.batch_group_id &&
            sp.stage_id == chain->stage_ids[1]) { stage2 = &sp; break; }
      if (!stage2)
        return Status::Error("BATCH_STAGE_MISSING",
                             group.batch_group_id + ":" + chain->stage_ids[1]);
      std::unordered_map<CameraId, GstElement*> s2_outputs;
      st = BuildBatchCascadeStage(*stage2, group, stage_outputs, pipeline, reg,
                                  &s2_outputs);
      if (!st.ok) return st;
      stage_outputs = std::move(s2_outputs);
    }

    for (const auto& bs : group.streams) {
      auto it = stage_outputs.find(bs.camera_id);
      if (it == stage_outputs.end())
        return Status::Error("BATCH_TERMINAL_INPUT_MISSING", bs.camera_id);
      guint max_roi = 0;
      for (const auto& sid : chain->stage_ids) {
        for (const auto& sp : plan.stage_plans) {
          if (sp.batch_group_id == group.batch_group_id && sp.stage_id == sid)
            max_roi += (guint)((sp.model_postprocess.results > 0)
                                 ? sp.model_postprocess.results : 0);
        }
      }
      const char* roi_env = g_getenv("AIBOX_B2_MAX_ROI");
      if (roi_env && roi_env[0])
        max_roi = (guint)atoi(roi_env);
      st = BuildBatchPerCameraTerminal(bs.camera_id, it->second, pipeline, reg,
                                       max_roi);
      if (!st.ok) return st;
    }

    fprintf(stderr,
            "[BATCH_GROUP] group=%s batch_size=%u split=none "
            "bypass=pre-batch-tee stages=%zu\n",
            group.batch_group_id.c_str(), group.batch_size,
            chain->stage_ids.size());
    for (const auto& bs : group.streams)
      fprintf(stderr,
              "[BATCH_GROUP]   slot%u stream_id=%u camera=%s\n",
              bs.slot, bs.stream_id, bs.camera_id.c_str());
  }

  return Status::Ok();
}

Status GstPipelineBuilder::BuildBatchDecodeStream(
    const StreamPlan& sp, const CapsString& ingress_caps,
    GstElement* pipeline, RuntimeRegistry* reg,
    const std::string& batch_group_id, guint batch_stream_id) const {
  std::string err;
  const bool is_hw = IsV4l2HwDecoder(sp.decode.decoder);

  GstElement* src    = MakeElement("rtspsrc", "rtspsrc_batch_" + sp.camera_id, &err);
  GstElement* depay  = MakeElement(sp.decode.depay,
                                   sp.decode.depay + "_batch_" + sp.camera_id, &err);
  GstElement* parse  = MakeElement(sp.decode.parse,
                                   sp.decode.parse + "_batch_" + sp.camera_id, &err);
  GstElement* dec    = MakeElement(sp.decode.decoder,
                                   sp.decode.decoder + "_batch_" + sp.camera_id, &err);
  GstElement* conv   = is_hw ? nullptr
                             : MakeElement("videoconvert",
                                           "conv_batch_" + sp.camera_id, &err);
  GstElement* capsflt = MakeElement("capsfilter",
                                    "batch_ingress_caps_" + sp.camera_id, &err);
  GstElement* q_out  = MakeElement("queue", "queue_batch_dec_" + sp.camera_id, &err);
  if (!src || !depay || !parse || !dec || !capsflt || !q_out ||
      (!is_hw && !conv))
    return Status::Error("BATCH_DECODE_CREATE_FAILED", sp.camera_id + ": " + err);

  g_object_set(src, "location", sp.rtsp_url.c_str(),
               "latency", (gint)200,
               "drop-on-latency", TRUE,
               "timeout", (guint64)5000000,
               nullptr);
  if (!SetFlagsPropertyByNick(src, "protocols", "tcp", &err))
    return Status::Error("BATCH_RTSP_PROTOCOL_FAILED", err);

  if (is_hw) {
    if (!SetEnumPropertyByNick(dec, "capture-io-mode",
                               sp.decode.capture_io_mode.c_str(), &err))
      return Status::Error("DECODER_DMABUF_FAILED", err);
    if (!SetEnumPropertyByNick(dec, "output-io-mode",
                               sp.decode.output_io_mode.c_str(), &err))
      return Status::Error("DECODER_DMABUF_FAILED", err);
  }
  if (sp.decode.rtp_wait_for_keyframe)
    g_object_set(depay, "wait-for-keyframe", TRUE, nullptr);
  if (sp.decode.rtp_request_keyframe)
    g_object_set(depay, "request-keyframe", TRUE, nullptr);

  const CapsString caps_str = !ingress_caps.empty()
      ? ingress_caps : CapsString("video/x-raw,format=NV12");
  if (!SetCapsFilter(capsflt, caps_str, &err))
    return Status::Error("BATCH_INGRESS_CAPS_FAILED", err);

  ConfigureBoundedQueue(q_out, 10, 0);

  gst_bin_add_many(GST_BIN(pipeline), src, depay, parse, dec, capsflt, q_out,
                   nullptr);
  if (conv) gst_bin_add(GST_BIN(pipeline), conv);

  std::vector<GstElement*> chain = {depay, parse, dec};
  if (!is_hw && conv) chain.push_back(conv);
  chain.push_back(capsflt);
  chain.push_back(q_out);
  if (!LinkElements(chain, &err))
    return Status::Error("BATCH_DECODE_LINK_FAILED", sp.camera_id + ": " + err);

  auto* ctx = new RtspPadCtx{sp.camera_id, depay};
  g_signal_connect(src, "pad-added", G_CALLBACK(OnRtspPadAdded), ctx);
  RuntimeRegistry::RtspPadLinkCtx rctx;
  rctx.camera_id = sp.camera_id;
  rctx.depay = depay;
  reg->rtsp_pad_contexts.push_back(rctx);

  {
    GstPad* q_sink = gst_element_get_static_pad(q_out, "sink");
    if (q_sink) {
      auto* tag_ctx = g_new0(FrameTagCtx, 1);
      tag_ctx->camera_id = g_strdup(sp.camera_id.c_str());
      tag_ctx->batch_group_id = g_strdup(batch_group_id.c_str());
      tag_ctx->batch_stream_id = batch_stream_id;
      gst_pad_add_probe(q_sink, GST_PAD_PROBE_TYPE_BUFFER,
                        OnFrameTagProbe, tag_ctx, frame_tag_ctx_free);
      reg->frame_count_ctx_in.push_back(tag_ctx);
      gst_object_unref(q_sink);
    }
  }

  reg->stream_source_by_camera[sp.camera_id] = src;
  reg->stream_queue_by_camera[sp.camera_id]  = q_out;
  reg->camera_id_by_rtsp_url[sp.rtsp_url]    = sp.camera_id;
  reg->decode_src_by_camera[sp.camera_id]    = q_out;
  fprintf(stderr,
          "[BATCH_DECODE] camera=%s group=%s stream_id=%u caps=%s "
          "(no rateadapt)\n",
          sp.camera_id.c_str(), batch_group_id.c_str(), batch_stream_id,
          caps_str.c_str());
  return Status::Ok();
}

static void ConfigureBatchSplit(GstElement* split, const BatchGroupPlan& group) {
  const CapsString mono_caps = !group.input_caps.empty()
      ? group.input_caps : CapsString("video/x-raw,format=NV12");
  gchar* cam0 = nullptr;
  gchar* cam1 = nullptr;
  for (const auto& bs : group.streams) {
    if (bs.stream_id == 0) cam0 = g_strdup(bs.camera_id.c_str());
    if (bs.stream_id == 1) cam1 = g_strdup(bs.camera_id.c_str());
  }
  g_object_set(split, "video-caps", mono_caps.c_str(),
               "camera-0", cam0 ? cam0 : "",
               "camera-1", cam1 ? cam1 : "", nullptr);
  g_free(cam0);
  g_free(cam1);
}

Status GstPipelineBuilder::BuildBatchStage(
    const StagePlan& stage, const BatchGroupPlan& group,
    const std::unordered_map<CameraId, GstElement*>& video_inputs,
    GstElement* pipeline, RuntimeRegistry* reg,
    std::unordered_map<CameraId, GstElement*>* stage_outputs) const {
  std::string err;
  const std::string gsfx = stage.stage_id + "_" + group.batch_group_id;

  fprintf(stderr,
          "[BATCH_STAGE] group=%s stage=%s ordinal=%d batch=%d htp=%d "
          "split=none bypass=pre-batch-tee leaky=downstream credit=none\n",
          group.batch_group_id.c_str(), stage.stage_id.c_str(),
          stage.stage_index, stage.model_batch,
          ResolveHtpDeviceId(stage.stage_index));

  GstElement* batch = MakeElement(group.batch_factory, "qtibatch_" + gsfx, &err);
  GstElement* q_lk  = MakeElement("queue", "q_batch_leaky_" + gsfx, &err);
  GstElement* q_in  = MakeElement("queue", "q_batch_in_" + gsfx, &err);
  GstElement* conv  = MakeElement(stage.converter_factory, stage.converter_name, &err);
  GstElement* q_c   = MakeElement("queue", "q_conv_out_" + gsfx, &err);
  GstElement* qnn   = MakeElement(stage.inference_factory, stage.inference_name, &err);
  GstElement* q_n   = MakeElement("queue", "q_qnn_out_" + gsfx, &err);
  GstElement* demux = MakeElement(group.demux_factory, "qtimldemux_" + gsfx, &err);
  if (!batch || !q_lk || !q_in || !conv || !q_c || !qnn || !q_n || !demux)
    return Status::Error("BATCH_STAGE_CREATE_FAILED", gsfx + ": " + err);

  g_object_set(batch, "moving-window-size",
               (guint)group.moving_window_size, nullptr);
  // q_lk default: leaky=downstream (shed packed batches QNN can't consume;
  // isolates a slow/bursty stream from stalling the shared batch).
  // AIBOX_B16_QLK_NOLEAKY=1: non-leaky backpressure — drops move to the RTSP
  // ingress (rtspsrc drop-on-latency), IN approaches OUT, no wasted 50MB batch
  // packing. Does NOT raise QNN throughput; only makes retention interpretable.
  const char* qlk_bp_env = g_getenv("AIBOX_B16_QLK_NOLEAKY");
  const bool qlk_backpressure = qlk_bp_env && qlk_bp_env[0] == '1';
  ConfigureBoundedQueue(q_lk, qlk_backpressure ? 4 : 2, qlk_backpressure ? 0 : 2);
  fprintf(stderr, "[BATCH_STAGE] q_lk leaky=%s size=%d\n",
          qlk_backpressure ? "none(backpressure)" : "downstream",
          qlk_backpressure ? 4 : 2);
  ConfigureBoundedQueue(q_in, 1, 0);
  ConfigureBoundedQueue(q_c, 1, 0);
  ConfigureBoundedQueue(q_n, 1, 0);

  GstElement* delay = nullptr;
  const char* sleep_ms_env = g_getenv("AIBOX_B2_OVERLOAD_SLEEP_MS");
  const gint sleep_ms = sleep_ms_env ? atoi(sleep_ms_env) : 0;
  if (sleep_ms > 0) {
    delay = MakeElement("identity", "qnn_delay_" + gsfx, &err);
    if (!delay) return Status::Error("BATCH_DELAY_CREATE_FAILED", err);
    const guint sleep_us = (guint)sleep_ms * 1000u;
    g_object_set(delay, "sync", FALSE, "sleep-time", sleep_us, nullptr);
    fprintf(stderr,
            "[BATCH_STAGE] test overload identity sleep-time=%dms (%u us)\n",
            sleep_ms, sleep_us);
  }

  gst_bin_add_many(GST_BIN(pipeline), batch, q_lk, q_in, conv, q_c, qnn,
                   q_n, demux, nullptr);
  if (delay) gst_bin_add(GST_BIN(pipeline), delay);

  if (!gst_element_link(batch, q_lk))
    return Status::Error("BATCH_LEAKY_LINK_FAILED", gsfx);
  if (delay) {
    if (!gst_element_link(q_lk, delay) || !gst_element_link(delay, q_in))
      return Status::Error("BATCH_DELAY_LINK_FAILED", gsfx);
  } else if (!gst_element_link(q_lk, q_in)) {
    return Status::Error("BATCH_LEAKY_IN_LINK_FAILED", gsfx);
  }
  if (!gst_element_link_many(q_in, conv, q_c, qnn, q_n, demux, nullptr))
    return Status::Error("BATCH_CHAIN_LINK_FAILED", gsfx);

  Status st = ConfigQtimlVConverter(conv, stage);
  if (!st.ok) return st;
  if (stage.inference_factory == "qtimlqnn") {
    st = ConfigQtimlQnn(qnn, stage);
  } else if (stage.inference_factory == "qtimlsnpe") {
    st = ConfigQtimlSnpe(qnn, stage);
  } else {
    return Status::Error("UNKNOWN_INFERENCE_FACTORY",
                         stage.stage_id + ": " + stage.inference_factory);
  }
  if (!st.ok) return st;

  AttachStageLatProbes(conv, stage.converter_name, reg);
  AttachStageLatProbes(qnn, stage.inference_name, reg);

  BatchGroupRuntime& br = reg->batch_group_runtime[group.batch_group_id];
  br.batch_split = nullptr;
  install_batch_src_probe(batch, "qtibatch_" + gsfx,
                          (guint)((1u << group.batch_size) - 1), &br);

  std::unordered_map<std::string, GstPad*> batch_sinks;
  std::unordered_map<std::string, GstPad*> demux_srcs;
  for (const auto& bs : group.streams) {
    GstPad* batch_sink =
        gst_element_request_pad_simple(batch, bs.batch_sink_pad_name.c_str());
    if (!batch_sink)
      return Status::Error("BATCH_SINK_PAD_REQUEST_FAILED",
                           gsfx + ":" + bs.batch_sink_pad_name);
    {
      auto* sctx = new R0SinkCtx();
      sctx->label = std::string("qtibatch.") + bs.batch_sink_pad_name +
                    " cam=" + bs.camera_id;
      gst_pad_add_probe(batch_sink, GST_PAD_PROBE_TYPE_BUFFER, OnR0SinkProbe,
                        sctx, [](gpointer d) { delete static_cast<R0SinkCtx*>(d); });
    }
    reg->request_pad_handles.emplace_back(batch, batch_sink);
    batch_sinks[bs.camera_id] = batch_sink;
    gst_object_unref(batch_sink);

    GstPad* demux_src =
        gst_element_request_pad_simple(demux, bs.demux_src_pad_name.c_str());
    if (!demux_src)
      return Status::Error("DEMUX_SRC_PAD_REQUEST_FAILED",
                           gsfx + ":" + bs.demux_src_pad_name);
    reg->request_pad_handles.emplace_back(demux, demux_src);
    demux_srcs[bs.camera_id] = demux_src;
    gst_object_unref(demux_src);
  }

  for (const auto& bs : group.streams) {
    auto vit = video_inputs.find(bs.camera_id);
    if (vit == video_inputs.end() || !vit->second)
      return Status::Error("BATCH_VIDEO_INPUT_MISSING", gsfx + ":" + bs.camera_id);
    const std::string csfx = gsfx + "_" + bs.camera_id;

    GstElement* tee   = MakeElement("tee", "tee_batch_" + csfx, &err);
    GstElement* q_inf = MakeElement("queue", "q_inf_" + csfx, &err);
    GstElement* q_byp = MakeElement("queue", "q_byp_" + csfx, &err);
    GstElement* q_txt = MakeElement("queue", "q_text_" + csfx, &err);
    GstElement* mmux  = MakeElement("qtimetamux", "metamux_" + csfx, &err);
    GstElement* q_res = MakeElement("queue", "q_result_" + csfx, &err);
    GstElement* post  = MakeElement(stage.postprocess_factory,
                                    stage.postprocess_name + "_" + bs.camera_id, &err);
    GstElement* postcaps = MakeElement("capsfilter", "caps_post_out_" + csfx, &err);
    if (!tee || !q_inf || !q_byp || !q_txt || !mmux || !q_res || !post ||
        !postcaps)
      return Status::Error("BATCH_PERCAM_CREATE_FAILED", csfx + ": " + err);

    // q_inf non-leaky: qtibatch backpressure. q_byp leaky: appsink must not
    // stall the batch arm.
    ConfigureBoundedQueue(q_inf, 2, 0);
    ConfigureBoundedQueue(q_byp, 4, 2);
    ConfigureBoundedQueue(q_txt, 10, 0);
    ConfigureBoundedQueue(q_res, 2, 0);
    gst_util_set_object_arg(G_OBJECT(mmux), "mode", "sync");
    g_object_set(mmux, "latency", (guint64)(2 * GST_SECOND), nullptr);

    GstCaps* pcaps = gst_caps_from_string(stage.postprocess_output_caps.c_str());
    if (!pcaps)
      return Status::Error("BATCH_POSTCAPS_PARSE_FAILED", csfx);
    g_object_set(postcaps, "caps", pcaps, nullptr);
    gst_caps_unref(pcaps);

    gst_bin_add_many(GST_BIN(pipeline), tee, q_inf, q_byp, q_txt, mmux, q_res,
                     post, postcaps, nullptr);

    GstPad* qdec_src = gst_element_get_static_pad(vit->second, "src");
    GstPad* tee_sink = gst_element_get_static_pad(tee, "sink");
    if (!qdec_src || !tee_sink ||
        gst_pad_link(qdec_src, tee_sink) != GST_PAD_LINK_OK) {
      if (qdec_src) gst_object_unref(qdec_src);
      if (tee_sink) gst_object_unref(tee_sink);
      return Status::Error("BATCH_DECODE_TO_TEE_FAILED", csfx);
    }
    gst_object_unref(qdec_src);
    gst_object_unref(tee_sink);

    auto bsi = batch_sinks.find(bs.camera_id);
    if (bsi == batch_sinks.end())
      return Status::Error("BATCH_SINK_LOOKUP_FAILED", csfx);
    GstPad* tee_inf = gst_element_request_pad_simple(tee, "src_%u");
    GstPad* qinf_sink = gst_element_get_static_pad(q_inf, "sink");
    GstPad* qinf_src = gst_element_get_static_pad(q_inf, "src");
    if (!tee_inf || !qinf_sink || !qinf_src ||
        gst_pad_link(tee_inf, qinf_sink) != GST_PAD_LINK_OK ||
        gst_pad_link(qinf_src, bsi->second) != GST_PAD_LINK_OK) {
      if (tee_inf) gst_object_unref(tee_inf);
      if (qinf_sink) gst_object_unref(qinf_sink);
      if (qinf_src) gst_object_unref(qinf_src);
      return Status::Error("BATCH_TEE_TO_QTIBATCH_FAILED", csfx);
    }
    reg->request_pad_handles.emplace_back(tee, tee_inf);
    gst_object_unref(tee_inf);
    gst_object_unref(qinf_sink);
    gst_object_unref(qinf_src);

    GstPad* tee_byp = gst_element_request_pad_simple(tee, "src_%u");
    GstPad* qbyp_sink = gst_element_get_static_pad(q_byp, "sink");
    if (!tee_byp || !qbyp_sink ||
        gst_pad_link(tee_byp, qbyp_sink) != GST_PAD_LINK_OK) {
      if (tee_byp) gst_object_unref(tee_byp);
      if (qbyp_sink) gst_object_unref(qbyp_sink);
      return Status::Error("BYPASS_VIDEO_LINK_FAILED", csfx);
    }
    reg->request_pad_handles.emplace_back(tee, tee_byp);
    gst_object_unref(tee_byp);
    gst_object_unref(qbyp_sink);
    if (!gst_element_link(q_byp, mmux))
      return Status::Error("BYPASS_LINK_FAILED", csfx);

    auto dsi = demux_srcs.find(bs.camera_id);
    if (dsi == demux_srcs.end())
      return Status::Error("DEMUX_SRC_LOOKUP_FAILED", csfx);
    GstPad* qres_sink = gst_element_get_static_pad(q_res, "sink");
    if (!qres_sink || gst_pad_link(dsi->second, qres_sink) != GST_PAD_LINK_OK) {
      if (qres_sink) gst_object_unref(qres_sink);
      return Status::Error("DEMUX_LINK_FAILED", csfx);
    }
    gst_object_unref(qres_sink);
    if (!gst_element_link_many(q_res, post, postcaps, nullptr))
      return Status::Error("POST_LINK_FAILED", csfx);

    st = ConfigQtimlPostProcess(post, stage);
    if (!st.ok) return st;
    AttachStageLatProbes(post, stage.postprocess_name + "_" + bs.camera_id, reg);

    st = LinkPostOutputToMetamux(postcaps, q_txt, mmux, csfx);
    if (!st.ok) return st;

    const std::string gskey = group.batch_group_id + ":" + stage.stage_id;
    reg->batch_by_group_stage[gskey] = batch;
    reg->converter_by_group_stage[gskey] = conv;
    reg->qnn_by_group_stage[gskey] = qnn;
    reg->demux_by_group_stage[gskey] = demux;
    reg->postprocess_by_stage_camera[stage.stage_id + ":" + bs.camera_id] = post;
    reg->metamux_by_stage_camera[stage.stage_id + ":" + bs.camera_id] = mmux;
    (*stage_outputs)[bs.camera_id] = mmux;
  }

  return Status::Ok();
}

Status GstPipelineBuilder::BuildBatchPerCameraTerminal(
    const CameraId& camera_id, GstElement* metamux,
    GstElement* pipeline, RuntimeRegistry* reg, guint max_roi) const {
  std::string err;
  GstElement* q_out   = MakeElement("queue", "queue_terminal_" + camera_id, &err);
  GstElement* appsink = MakeElement("appsink", "appsink_terminal_" + camera_id, &err);
  if (!q_out || !appsink)
    return Status::Error("BATCH_TERMINAL_CREATE_FAILED", camera_id + ": " + err);

  ConfigureBoundedQueue(q_out, 10, 0);
  g_object_set(appsink, "sync", FALSE, "async", FALSE,
               "max-buffers", (guint)8, "emit-signals", TRUE, nullptr);
  g_object_set_data(G_OBJECT(appsink), "aibox-max-roi",
                    GUINT_TO_POINTER(max_roi));
  fprintf(stderr, "[BATCH_ROI] camera=%s max_roi=%u\n",
          camera_id.c_str(), max_roi);

  gst_bin_add_many(GST_BIN(pipeline), q_out, appsink, nullptr);
  GstElement* term_src = metamux;
  if (NowebFpsEnabled(opts_)) {
    GstElement* tee_fps = MakeElement("tee", "tee_fps_" + camera_id, &err);
    if (!tee_fps)
      return Status::Error("BATCH_FPS_TEE_FAILED", camera_id + ": " + err);
    gst_bin_add(GST_BIN(pipeline), tee_fps);
    if (!gst_element_link(metamux, tee_fps))
      return Status::Error("BATCH_FPS_TEE_LINK_FAILED", camera_id);
    term_src = tee_fps;
  }
  if (!gst_element_link_many(term_src, q_out, appsink, nullptr))
    return Status::Error("BATCH_TERMINAL_LINK_FAILED", camera_id);

  if (NowebFpsEnabled(opts_)) {
    GstPad* tee_src = gst_element_request_pad_simple(term_src, "src_%u");
    if (!tee_src)
      return Status::Error("BATCH_FPS_PAD_FAILED", camera_id);
    Status st = AttachFpsDisplaySink(pipeline, tee_src, camera_id,
                                     "batch", reg);
    gst_object_unref(tee_src);
    if (!st.ok) return st;
  }

  RuntimeRegistry::AppSinkEntry entry;
  entry.stage_id  = "__terminal__";
  entry.branch_id = "batch_terminal_" + camera_id;
  entry.appsink   = appsink;
  reg->appsinks.push_back(entry);
  return Status::Ok();
}

Status GstPipelineBuilder::BuildBatchCascadeStage(
    const StagePlan& stage, const BatchGroupPlan& group,
    const std::unordered_map<CameraId, GstElement*>& prev_metamux,
    GstElement* pipeline, RuntimeRegistry* reg,
    std::unordered_map<CameraId, GstElement*>* stage_outputs) const {
  std::string err;
  const std::string gsfx = stage.stage_id + "_" + group.batch_group_id;

  fprintf(stderr,
          "[BATCH_CASCADE] group=%s stage=%s ordinal=%d batch=%d htp=%d "
          "lockstep=1 qtibatch_s2=1 split=aiboxbatchsplit credit=none\n",
          group.batch_group_id.c_str(), stage.stage_id.c_str(),
          stage.stage_index, stage.model_batch,
          ResolveHtpDeviceId(stage.stage_index));

  GstElement* batch = MakeElement(group.batch_factory, "qtibatch_" + gsfx, &err);
  GstElement* q_lk  = MakeElement("queue", "q_batch_leaky_" + gsfx, &err);
  GstElement* split = MakeElement("aiboxbatchsplit", "split_" + gsfx, &err);
  GstElement* q_in  = MakeElement("queue", "q_batch_in_" + gsfx, &err);
  GstElement* conv  = MakeElement(stage.converter_factory, stage.converter_name, &err);
  GstElement* q_c   = MakeElement("queue", "q_conv_out_" + gsfx, &err);
  GstElement* qnn   = MakeElement(stage.inference_factory, stage.inference_name, &err);
  GstElement* q_n   = MakeElement("queue", "q_qnn_out_" + gsfx, &err);
  GstElement* demux = MakeElement(group.demux_factory, "qtimldemux_" + gsfx, &err);
  if (!batch || !q_lk || !split || !q_in || !conv || !q_c || !qnn || !q_n || !demux)
    return Status::Error("BATCH_CASCADE_CREATE_FAILED", gsfx + ": " + err);

  g_object_set(batch, "moving-window-size",
               (guint)group.moving_window_size, nullptr);
  ConfigureBoundedQueue(q_lk, 2, 2);
  ConfigureBoundedQueue(q_in, 1, 0);
  ConfigureBoundedQueue(q_c, 1, 0);
  ConfigureBoundedQueue(q_n, 1, 0);
  ConfigureBatchSplit(split, group);

  gst_bin_add_many(GST_BIN(pipeline), batch, q_lk, split, q_in, conv, q_c, qnn,
                   q_n, demux, nullptr);
  if (!gst_element_link(batch, q_lk) ||
      !gst_element_link_pads(q_lk, "src", split, "sink") ||
      !gst_element_link_pads(split, "src", q_in, "sink") ||
      !gst_element_link_many(q_in, conv, q_c, qnn, q_n, demux, nullptr))
    return Status::Error("BATCH_CASCADE_CHAIN_LINK_FAILED", gsfx);

  Status st = ConfigQtimlVConverter(conv, stage);
  if (!st.ok) return st;
  if (stage.inference_factory == "qtimlqnn") {
    st = ConfigQtimlQnn(qnn, stage);
  } else if (stage.inference_factory == "qtimlsnpe") {
    st = ConfigQtimlSnpe(qnn, stage);
  } else {
    return Status::Error("UNKNOWN_INFERENCE_FACTORY",
                         stage.stage_id + ": " + stage.inference_factory);
  }
  if (!st.ok) return st;

  BatchGroupRuntime& br = reg->batch_group_runtime[group.batch_group_id];
  br.qtibatch_s2 = batch;
  br.batch_split_s2 = split;
  br.lockstep = std::make_shared<BatchLockstepBarrier>();
  batch_lockstep_init(br.lockstep.get());
  install_batch_src_probe(batch, "qtibatch_" + gsfx,
                          (guint)((1u << group.batch_size) - 1), &br);

  std::unordered_map<std::string, GstPad*> batch_sinks;
  std::unordered_map<std::string, GstPad*> demux_srcs;
  std::unordered_map<std::string, GstPad*> video_pads;
  for (const auto& bs : group.streams) {
    GstPad* batch_sink =
        gst_element_request_pad_simple(batch, bs.batch_sink_pad_name.c_str());
    if (!batch_sink)
      return Status::Error("BATCH_SINK_PAD_REQUEST_FAILED",
                           gsfx + ":" + bs.batch_sink_pad_name);
    {
      auto* sctx = new R0SinkCtx();
      sctx->label = std::string("qtibatch_s2.") + bs.batch_sink_pad_name +
                    " cam=" + bs.camera_id;
      gst_pad_add_probe(batch_sink, GST_PAD_PROBE_TYPE_BUFFER, OnR0SinkProbe,
                        sctx, [](gpointer d) { delete static_cast<R0SinkCtx*>(d); });
    }
    reg->request_pad_handles.emplace_back(batch, batch_sink);
    batch_sinks[bs.camera_id] = batch_sink;
    gst_object_unref(batch_sink);

    const std::string vname = "video_" + std::to_string(bs.stream_id);
    GstPad* vpad = gst_element_request_pad_simple(split, vname.c_str());
    if (!vpad)
      return Status::Error("BATCH_SPLIT_VIDEO_PAD_FAILED", gsfx + ":" + vname);
    reg->request_pad_handles.emplace_back(split, vpad);
    video_pads[bs.camera_id] = vpad;
    gst_object_unref(vpad);

    GstPad* demux_src =
        gst_element_request_pad_simple(demux, bs.demux_src_pad_name.c_str());
    if (!demux_src)
      return Status::Error("DEMUX_SRC_PAD_REQUEST_FAILED",
                           gsfx + ":" + bs.demux_src_pad_name);
    reg->request_pad_handles.emplace_back(demux, demux_src);
    demux_srcs[bs.camera_id] = demux_src;
    gst_object_unref(demux_src);
  }

  for (const auto& bs : group.streams) {
    auto mit = prev_metamux.find(bs.camera_id);
    if (mit == prev_metamux.end() || !mit->second)
      return Status::Error("BATCH_S1_METAMUX_MISSING", gsfx + ":" + bs.camera_id);
    const std::string csfx = gsfx + "_" + bs.camera_id;

    GstElement* q_s2in = MakeElement("queue", "q_s2in_" + csfx, &err);
    GstElement* lock_sink = MakeElement("fakesink", "lockstep_hold_" + csfx, &err);
    GstElement* q_byp = MakeElement("queue", "q_byp_" + csfx, &err);
    GstElement* q_txt = MakeElement("queue", "q_text_" + csfx, &err);
    GstElement* mmux  = MakeElement("qtimetamux", "metamux_" + csfx, &err);
    GstElement* q_res = MakeElement("queue", "q_result_" + csfx, &err);
    GstElement* post  = MakeElement(stage.postprocess_factory,
                                    stage.postprocess_name + "_" + bs.camera_id, &err);
    GstElement* postcaps = MakeElement("capsfilter", "caps_post_out_" + csfx, &err);
    if (!q_s2in || !lock_sink || !q_byp || !q_txt || !mmux || !q_res || !post ||
        !postcaps)
      return Status::Error("BATCH_CASCADE_PERCAM_CREATE_FAILED",
                           csfx + ": " + err);

    ConfigureBoundedQueue(q_s2in, 2, 0);
    ConfigureBoundedQueue(q_byp, 4, 0);
    ConfigureBoundedQueue(q_txt, 10, 0);
    ConfigureBoundedQueue(q_res, 2, 0);
    gst_util_set_object_arg(G_OBJECT(mmux), "mode", "sync");
    g_object_set(mmux, "latency", (guint64)(2 * GST_SECOND), nullptr);
    g_object_set(lock_sink, "sync", FALSE, "async", FALSE, nullptr);

    GstCaps* pcaps = gst_caps_from_string(stage.postprocess_output_caps.c_str());
    if (!pcaps)
      return Status::Error("BATCH_POSTCAPS_PARSE_FAILED", csfx);
    g_object_set(postcaps, "caps", pcaps, nullptr);
    gst_caps_unref(pcaps);

    gst_bin_add_many(GST_BIN(pipeline), q_s2in, lock_sink, q_byp, q_txt, mmux,
                     q_res, post, postcaps, nullptr);

    if (!gst_element_link(mit->second, q_s2in))
      return Status::Error("S1_METAMUX_S2IN_LINK_FAILED", csfx);
    if (!gst_element_link(q_s2in, lock_sink))
      return Status::Error("LOCKSTEP_HOLD_LINK_FAILED", csfx);
    auto bsi = batch_sinks.find(bs.camera_id);
    if (bsi == batch_sinks.end())
      return Status::Error("BATCH_SINK_LOOKUP_FAILED", csfx);
    GstPad* qs2_src = gst_element_get_static_pad(q_s2in, "src");
    if (!qs2_src)
      return Status::Error("LOCKSTEP_SRC_MISSING", csfx);
    if (!br.lockstep)
      return Status::Error("LOCKSTEP_BARRIER_MISSING", gsfx);
    auto* slot = new BatchLockstepSlot;
    slot->bar = br.lockstep.get();
    slot->slot = bs.stream_id <= 1 ? bs.stream_id : bs.slot;
    slot->label = group.batch_group_id.c_str();
    const guint slot_i = slot->slot <= 1 ? slot->slot : 0;
    batch_lockstep_set_dest(br.lockstep.get(), slot_i, bsi->second);
    br.lockstep_pad[slot_i] = GST_PAD(gst_object_ref(qs2_src));
    br.lockstep_probe_id[slot_i] = gst_pad_add_probe(
        qs2_src,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        OnBatchLockstepCollectProbe, slot,
        [](gpointer d) { delete static_cast<BatchLockstepSlot*>(d); });
    gst_object_unref(qs2_src);

    auto vpi = video_pads.find(bs.camera_id);
    if (vpi == video_pads.end())
      return Status::Error("BATCH_VIDEO_PAD_LOOKUP_FAILED", csfx);
    GstPad* qbyp_sink = gst_element_get_static_pad(q_byp, "sink");
    if (!qbyp_sink || gst_pad_link(vpi->second, qbyp_sink) != GST_PAD_LINK_OK) {
      if (qbyp_sink) gst_object_unref(qbyp_sink);
      return Status::Error("BYPASS_VIDEO_LINK_FAILED", csfx);
    }
    gst_object_unref(qbyp_sink);
    if (!gst_element_link(q_byp, mmux))
      return Status::Error("BYPASS_LINK_FAILED", csfx);

    auto dsi = demux_srcs.find(bs.camera_id);
    if (dsi == demux_srcs.end())
      return Status::Error("DEMUX_SRC_LOOKUP_FAILED", csfx);
    GstPad* qres_sink = gst_element_get_static_pad(q_res, "sink");
    if (!qres_sink || gst_pad_link(dsi->second, qres_sink) != GST_PAD_LINK_OK) {
      if (qres_sink) gst_object_unref(qres_sink);
      return Status::Error("DEMUX_LINK_FAILED", csfx);
    }
    gst_object_unref(qres_sink);
    if (!gst_element_link_many(q_res, post, postcaps, nullptr))
      return Status::Error("POST_LINK_FAILED", csfx);

    st = ConfigQtimlPostProcess(post, stage);
    if (!st.ok) return st;
    st = LinkPostOutputToMetamux(postcaps, q_txt, mmux, csfx);
    if (!st.ok) return st;

    const std::string gskey = group.batch_group_id + ":" + stage.stage_id;
    reg->batch_by_group_stage[gskey] = batch;
    reg->converter_by_group_stage[gskey] = conv;
    reg->qnn_by_group_stage[gskey] = qnn;
    reg->demux_by_group_stage[gskey] = demux;
    reg->postprocess_by_stage_camera[stage.stage_id + ":" + bs.camera_id] = post;
    reg->metamux_by_stage_camera[stage.stage_id + ":" + bs.camera_id] = mmux;
    (*stage_outputs)[bs.camera_id] = mmux;
  }

  return Status::Ok();
}

Status GstPipelineBuilder::AttachFpsDisplaySink(
    GstElement* pipeline, GstPad* src_pad, const std::string& camera_id,
    const std::string& elem_suffix, RuntimeRegistry* reg) const {
  if (!pipeline || !src_pad || !reg)
    return Status::Error("FPS_ATTACH_FAILED", camera_id + ": null arg");

  std::string safe_cam = FpsSafeName(camera_id);
  std::string safe_sfx = FpsSafeName(elem_suffix);

  GstElement* fsink = gst_element_factory_make("fakesink", nullptr);
  if (!fsink)
    return Status::Error("FAKESINK_FAILED", camera_id);
  g_object_set(fsink, "sync", FALSE, "async", FALSE, nullptr);

  std::string fps_name = "fps_" + safe_cam + "_" + safe_sfx;
  GstElement* fps = gst_element_factory_make("fpsdisplaysink", fps_name.c_str());
  if (!fps) {
    gst_object_unref(fsink);
    return Status::Error("FPSDISPLAYSINK_FAILED", camera_id);
  }
  g_object_set(fps,
      "video-sink", fsink,
      "text-overlay", FALSE,
      "sync", FALSE,
      "signal-fps-measurements", TRUE,
      "silent", TRUE,
      "fps-update-interval", 500,
      nullptr);

  auto *fps_ctx = new FpsCsvCtx{camera_id};
  g_signal_connect(fps, "fps-measurements",
      G_CALLBACK(OnFpsMeasurement), fps_ctx);

  std::string q_name = "q_fps_" + safe_cam + "_" + safe_sfx;
  GstElement* q_fps = gst_element_factory_make("queue", q_name.c_str());
  if (!q_fps) {
    gst_object_unref(fps);
    return Status::Error("QUEUE_FAILED", "q_fps " + camera_id);
  }
  g_object_set(q_fps,
      "max-size-buffers", (guint)4,
      "max-size-time",    (guint64)0,
      "max-size-bytes",   (guint)0,
      nullptr);

  gst_bin_add_many(GST_BIN(pipeline), q_fps, fps, nullptr);

  GstPad* q_sink   = gst_element_get_static_pad(q_fps, "sink");
  GstPad* q_src    = gst_element_get_static_pad(q_fps, "src");
  GstPad* fps_sink = gst_element_get_static_pad(fps, "sink");
  GstPadLinkReturn lr1 = gst_pad_link(src_pad, q_sink);
  GstPadLinkReturn lr2 = gst_pad_link(q_src, fps_sink);
  gst_object_unref(q_sink);
  gst_object_unref(q_src);
  gst_object_unref(fps_sink);
  if (lr1 != GST_PAD_LINK_OK || lr2 != GST_PAD_LINK_OK)
    return Status::Error("LINK_FAILED",
        camera_id + " → queue → fpsdisplaysink " + elem_suffix);

  // Mux composite probe is not a per-camera IN/OUT row.
  if (camera_id.rfind("mux_", 0) != 0) {
    auto *out_ctx = new FrameCountOutCtx{camera_id, 0};
    GstPad *fs_pad = gst_element_get_static_pad(fsink, "sink");
    if (fs_pad) {
      gst_pad_add_probe(fs_pad, GST_PAD_PROBE_TYPE_BUFFER,
                        OnFrameOutProbe, out_ctx, nullptr);
      gst_object_unref(fs_pad);
    }
    reg->frame_count_ctx_out.push_back(out_ctx);
  }

  fprintf(stderr,
          "[display] %s cam=%s → queue → fpsdisplaysink(video-sink=fakesink)\n",
          elem_suffix.c_str(), camera_id.c_str());
  return Status::Ok();
}

Status GstPipelineBuilder::BuildNowebFpsDisplayBranch(
    GstElement* pipeline, GstElement* tee_fps, const TimeMuxGroupPlan& tmux,
    RuntimeRegistry* reg) const {
  std::string err;
  const std::string& gid = tmux.time_mux_group_id;
  const std::string mux_id = "mux_" + gid;

  GstPad* mux_tee_src = gst_element_request_pad_simple(tee_fps, "src_%u");
  if (!mux_tee_src)
    return Status::Error("NOWEB_FPS_PAD_FAILED", "mux " + gid);
  Status st = AttachFpsDisplaySink(pipeline, mux_tee_src, mux_id, gid, reg);
  gst_object_unref(mux_tee_src);
  if (!st.ok) return st;

  GstElement* demux = MakeElement("aiboxstreamdemux", "demux_fps_" + gid, &err);
  if (!demux)
    return Status::Error("DEMUX_CREATE_FAILED", err);
  g_object_set(demux,
               "max-srcs", (guint)tmux.stream_inputs.size(),
               "output-fps", (guint)0,
               "silent", FALSE, nullptr);
  GstElement* q_pre = MakeElement("queue", "q_predemux_" + gid, &err);
  if (!q_pre)
    return Status::Error("QUEUE_FAILED", err);
  gst_bin_add_many(GST_BIN(pipeline), q_pre, demux, nullptr);
  if (!gst_element_link_many(tee_fps, q_pre, demux, nullptr))
    return Status::Error("LINK_FAILED", "tee_fps -> q_pre -> demux " + gid);

  int slot = 0;
  for (const auto& sip : tmux.stream_inputs) {
    gchar pad_name[16];
    g_snprintf(pad_name, sizeof(pad_name), "src_%d", slot);
    GstPad* demux_src = gst_element_request_pad_simple(demux, pad_name);
    if (!demux_src)
      return Status::Error("DEMUX_PAD_FAILED",
          gid + " src_" + std::to_string(slot));
    g_object_set_data_full(G_OBJECT(demux_src), "camera-id",
                           g_strdup(sip.camera_id.c_str()), g_free);
    st = AttachFpsDisplaySink(pipeline, demux_src, sip.camera_id, gid, reg);
    gst_object_unref(demux_src);
    if (!st.ok) return st;
    ++slot;
  }
  return Status::Ok();
}

}  // namespace aibox::pipeline
