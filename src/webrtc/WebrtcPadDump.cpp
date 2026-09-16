#include "aibox/webrtc/WebrtcPadDump.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include <gst/gst.h>
#include <gst/video/video.h>

namespace aibox::webrtc {
namespace {

struct ProbeCtx {
  std::string id;
  std::string out_dir;
  std::string elem_name;
  std::string pad_name;
  int skip = 40;
  int max_dump = 3;
  int seen = 0;
  int dumped = 0;
  bool caps_written = false;
  bool is_encoded = false;      // P5/P5b: H.264 bitstream
  bool wait_keyframe = false;   // gate until first IDR, then dump N frames
  bool got_keyframe = false;
  bool force_ku_sent = false;
  GstPad* pad = nullptr;
  // Upstream force-key-unit must be sent on encoder SRC (travels into enc),
  // not on enc sink (that would go further upstream away from enc).
  GstPad* enc_src_for_ku = nullptr;
  gulong probe_handle = 0;
};

std::mutex g_mu;
std::vector<ProbeCtx*> g_probes;
std::atomic<bool> g_installed{false};

static int EnvInt(const char* key, int def_v) {
  const char* v = std::getenv(key);
  if (!v || !*v) return def_v;
  return std::atoi(v);
}

static bool EnvEnabled() {
  const char* v = std::getenv("AIBOX_WEBRTC_PAD_DUMP");
  return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
               std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "yes") == 0);
}

static void MkDirP(const std::string& path) {
  // single-level mkdir is enough for our diag dirs; parents created by caller
  mkdir(path.c_str(), 0755);
}

static std::string CapsToString(GstCaps* caps) {
  if (!caps) return "(null)";
  gchar* s = gst_caps_to_string(caps);
  std::string out = s ? s : "(null)";
  g_free(s);
  return out;
}

static bool CapsHasDmaFeature(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return false;
  for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
    GstCapsFeatures* f = gst_caps_get_features(caps, i);
    if (!f) continue;
    if (gst_caps_features_contains(f, "memory:DMABuf") ||
        gst_caps_features_contains(f, "memory:GBM")) {
      return true;
    }
  }
  return false;
}

static bool BufferLooksDma(GstBuffer* buf) {
  if (!buf || gst_buffer_n_memory(buf) == 0) return false;
  GstMemory* mem = gst_buffer_peek_memory(buf, 0);
  if (!mem) return false;
  GstAllocator* alloc = mem->allocator;
  if (!alloc || !GST_OBJECT_NAME(alloc)) return false;
  const char* an = GST_OBJECT_NAME(alloc);
  // Heuristic without linking gstreamer-allocators: name often contains dmabuf/fd/gbm
  return (std::strstr(an, "dmabuf") != nullptr || std::strstr(an, "DmaBuf") != nullptr ||
          std::strstr(an, "gbm") != nullptr || std::strstr(an, "fd") != nullptr);
}

static void WriteCapsFile(ProbeCtx* ctx, GstCaps* caps, GstBuffer* buf) {
  if (ctx->caps_written) return;
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s_caps.txt", ctx->out_dir.c_str(),
                ctx->id.c_str());
  FILE* fp = std::fopen(path, "w");
  if (!fp) return;
  std::fprintf(fp, "probe=%s\n", ctx->id.c_str());
  std::fprintf(fp, "element=%s\n", ctx->elem_name.c_str());
  std::fprintf(fp, "pad=%s\n", ctx->pad_name.c_str());
  std::fprintf(fp, "caps=%s\n", CapsToString(caps).c_str());
  std::fprintf(fp, "caps_has_dma_feature=%d\n", CapsHasDmaFeature(caps) ? 1 : 0);
  std::fprintf(fp, "buffer_is_dmabuf_memory=%d\n",
               BufferLooksDma(buf) ? 1 : 0);
  if (buf) {
    std::fprintf(fp, "n_memory=%u\n", gst_buffer_n_memory(buf));
    std::fprintf(fp, "pts=%" GST_TIME_FORMAT "\n",
                 GST_TIME_ARGS(GST_BUFFER_PTS(buf)));
    std::fprintf(fp, "size=%zu\n", static_cast<size_t>(gst_buffer_get_size(buf)));
  }
  std::fclose(fp);
  ctx->caps_written = true;
  std::fprintf(stderr,
               "[pad-dump] %s caps written (dma_feat=%d dmabuf_mem=%d)\n",
               ctx->id.c_str(), CapsHasDmaFeature(caps) ? 1 : 0,
               BufferLooksDma(buf) ? 1 : 0);
}

static bool DumpRawYAsPgm(ProbeCtx* ctx, GstBuffer* buf, GstCaps* caps) {
  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) {
    std::fprintf(stderr, "[pad-dump] %s: video_info_from_caps failed\n",
                 ctx->id.c_str());
    return false;
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
    std::fprintf(stderr, "[pad-dump] %s: frame_map failed\n", ctx->id.c_str());
    return false;
  }

  const int w = GST_VIDEO_INFO_WIDTH(&info);
  const int h = GST_VIDEO_INFO_HEIGHT(&info);
  guint8* y = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s_f%02d_%dx%d.pgm",
                ctx->out_dir.c_str(), ctx->id.c_str(), ctx->dumped, w, h);
  FILE* fp = std::fopen(path, "wb");
  bool ok = false;
  if (fp) {
    std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
    for (int row = 0; row < h; ++row) {
      std::fwrite(y + row * stride, 1, static_cast<size_t>(w), fp);
    }
    std::fclose(fp);
    ok = true;
    std::fprintf(stderr, "[pad-dump] %s wrote %s\n", ctx->id.c_str(), path);
  }
  gst_video_frame_unmap(&frame);
  return ok;
}

static bool DumpEncoded(ProbeCtx* ctx, GstBuffer* buf) {
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s_f%02d.h264", ctx->out_dir.c_str(),
                ctx->id.c_str(), ctx->dumped);
  FILE* fp = std::fopen(path, "wb");
  if (!fp) return false;
  GstMapInfo map;
  if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
    std::fclose(fp);
    return false;
  }
  std::fwrite(map.data, 1, map.size, fp);
  gst_buffer_unmap(buf, &map);
  std::fclose(fp);

  // Also append to a single stream file for offline decode
  char stream_path[512];
  std::snprintf(stream_path, sizeof(stream_path), "%s/%s_stream.h264",
                ctx->out_dir.c_str(), ctx->id.c_str());
  FILE* sfp = std::fopen(stream_path, "ab");
  if (sfp) {
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
      std::fwrite(map.data, 1, map.size, sfp);
      gst_buffer_unmap(buf, &map);
    }
    std::fclose(sfp);
  }

  std::fprintf(stderr, "[pad-dump] %s wrote %s size=%zu\n", ctx->id.c_str(),
               path, static_cast<size_t>(gst_buffer_get_size(buf)));
  return true;
}

// Annex-B scan: true if AU contains IDR(5). SPS-only is not enough to paint a frame.
static bool H264HasIdr(GstBuffer* buf) {
  GstMapInfo map;
  if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return false;
  const guint8* d = map.data;
  const gsize n = map.size;
  bool hit = false;
  for (gsize i = 0; i + 4 < n; ++i) {
    int sc = 0;
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
      sc = 4;
    else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
      sc = 3;
    if (!sc) continue;
    gsize j = i + static_cast<gsize>(sc);
    if (j >= n) break;
    int nal = d[j] & 0x1f;
    if (nal == 5) {
      hit = true;
      break;
    }
    i = j;
  }
  gst_buffer_unmap(buf, &map);
  return hit;
}

static void MaybeForceKeyUnit(ProbeCtx* ctx) {
  if (ctx->force_ku_sent || !ctx->enc_src_for_ku) return;
  GstEvent* ev = gst_video_event_new_upstream_force_key_unit(
      GST_CLOCK_TIME_NONE, TRUE, 0);
  gboolean ok = gst_pad_send_event(ctx->enc_src_for_ku, ev);
  ctx->force_ku_sent = true;
  std::fprintf(stderr, "[pad-dump] %s force-key-unit sent=%d\n", ctx->id.c_str(),
               ok ? 1 : 0);
}

static GstPadProbeReturn OnBuffer(GstPad* /*pad*/, GstPadProbeInfo* info,
                                  gpointer user_data) {
  auto* ctx = static_cast<ProbeCtx*>(user_data);
  if (!(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  ctx->seen++;
  if (ctx->seen <= ctx->skip) return GST_PAD_PROBE_OK;
  if (ctx->dumped >= ctx->max_dump) return GST_PAD_PROBE_OK;

  // After skip window: request IDR so P5b can be decoded offline.
  if (ctx->is_encoded && ctx->wait_keyframe) MaybeForceKeyUnit(ctx);

  if (ctx->is_encoded && ctx->wait_keyframe && !ctx->got_keyframe) {
    if (!H264HasIdr(buf)) {
      // Keep waiting (and re-request KU every 30 buffers)
      if ((ctx->seen - ctx->skip) % 30 == 0) {
        ctx->force_ku_sent = false;
        MaybeForceKeyUnit(ctx);
      }
      return GST_PAD_PROBE_OK;
    }
    ctx->got_keyframe = true;
    std::fprintf(stderr, "[pad-dump] %s got IDR at seen=%d\n", ctx->id.c_str(),
                 ctx->seen);
  }

  GstCaps* caps = gst_pad_get_current_caps(ctx->pad);
  if (!caps) caps = gst_pad_get_allowed_caps(ctx->pad);
  WriteCapsFile(ctx, caps, buf);

  bool ok = false;
  if (ctx->is_encoded) {
    ok = DumpEncoded(ctx, buf);
  } else {
    if (caps) ok = DumpRawYAsPgm(ctx, buf, caps);
  }
  if (ok) ctx->dumped++;

  if (caps) gst_caps_unref(caps);

  if (ctx->dumped >= ctx->max_dump) {
    std::fprintf(stderr, "[pad-dump] %s done (%d frames)\n", ctx->id.c_str(),
                 ctx->dumped);
  }
  return GST_PAD_PROBE_OK;
}

static GstElement* FindByName(GstElement* pipeline, const char* name) {
  return gst_bin_get_by_name(GST_BIN(pipeline), name);
}

static GstElement* FindByPrefix(GstElement* pipeline, const char* prefix,
                                bool exclude_webrtc_demux) {
  GstIterator* it = gst_bin_iterate_recurse(GST_BIN(pipeline));
  GValue item = G_VALUE_INIT;
  GstElement* found = nullptr;
  while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
    GstElement* e = GST_ELEMENT(g_value_get_object(&item));
    const gchar* n = GST_OBJECT_NAME(e);
    if (n && std::strncmp(n, prefix, std::strlen(prefix)) == 0) {
      if (exclude_webrtc_demux &&
          std::strncmp(n, "demux_webrtc_", 13) == 0) {
        g_value_reset(&item);
        continue;
      }
      found = GST_ELEMENT(gst_object_ref(e));
      g_value_unset(&item);
      break;
    }
    g_value_reset(&item);
  }
  g_value_unset(&item);
  gst_iterator_free(it);
  return found;
}

static bool Attach(GstElement* pipeline, const std::string& id,
                   GstElement* elem, const char* pad_name, bool is_encoded,
                   const std::string& out_dir, int skip, int count,
                   bool wait_keyframe = false, GstPad* enc_src_for_ku = nullptr) {
  (void)pipeline;
  if (!elem) {
    std::fprintf(stderr, "[pad-dump] %s: element not found\n", id.c_str());
    return false;
  }
  GstPad* pad = gst_element_get_static_pad(elem, pad_name);
  if (!pad) {
    std::fprintf(stderr, "[pad-dump] %s: pad %s not found on %s\n", id.c_str(),
                 pad_name, GST_OBJECT_NAME(elem));
    gst_object_unref(elem);
    return false;
  }

  auto* ctx = new ProbeCtx();
  ctx->id = id;
  ctx->out_dir = out_dir;
  ctx->elem_name = GST_OBJECT_NAME(elem);
  ctx->pad_name = pad_name;
  ctx->skip = skip;
  ctx->max_dump = count;
  ctx->is_encoded = is_encoded;
  ctx->wait_keyframe = wait_keyframe;
  ctx->enc_src_for_ku =
      enc_src_for_ku ? GST_PAD(gst_object_ref(enc_src_for_ku)) : nullptr;
  ctx->pad = pad;
  ctx->probe_handle = gst_pad_add_probe(
      pad, GST_PAD_PROBE_TYPE_BUFFER, OnBuffer, ctx, nullptr);

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_probes.push_back(ctx);
  }
  std::fprintf(stderr,
               "[pad-dump] attached %s on %s:%s skip=%d count=%d wait_kf=%d\n",
               id.c_str(), ctx->elem_name.c_str(), pad_name, skip, count,
               wait_keyframe ? 1 : 0);
  gst_object_unref(elem);
  return true;
}

// Caller provides a stable probe snapshot. Never locks g_mu — avoids
// recursive deadlock when UninstallWebrtcPadDumps already holds it.
static void WriteSummaryFrom(const std::string& out_dir,
                             const std::vector<ProbeCtx*>& probes) {
  char path[512];
  std::snprintf(path, sizeof(path), "%s/REPORT.txt", out_dir.c_str());
  FILE* fp = std::fopen(path, "w");
  if (!fp) return;
  std::fprintf(fp,
               "WebRTC pad-dump summary\n"
               "Inspect each P*_fXX_*.pgm (Y plane) for boxes.\n"
               "P5 is H.264 annex-B; decode offline to check boxes.\n"
               "Compare P2 vs P2m before concluding demux make_writable.\n\n");
  for (auto* ctx : probes) {
    if (!ctx) continue;
    std::fprintf(fp,
                 "%s elem=%s pad=%s seen=%d dumped=%d caps_written=%d\n",
                 ctx->id.c_str(), ctx->elem_name.c_str(), ctx->pad_name.c_str(),
                 ctx->seen, ctx->dumped, ctx->caps_written ? 1 : 0);
  }
  std::fclose(fp);
}

static void WriteSummary(const std::string& out_dir) {
  std::vector<ProbeCtx*> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snapshot = g_probes;
  }
  WriteSummaryFrom(out_dir, snapshot);
}

}  // namespace

void MaybeInstallWebrtcPadDumps(GstElement* pipeline,
                                const aibox::pipeline::RuntimeRegistry& reg,
                                const std::string& out_dir_arg) {
  if (!EnvEnabled()) {
    std::fprintf(stderr,
                 "[pad-dump] disabled (set AIBOX_WEBRTC_PAD_DUMP=1 to enable)\n");
    return;
  }
  if (!pipeline) return;
  if (g_installed.exchange(true)) {
    std::fprintf(stderr, "[pad-dump] already installed\n");
    return;
  }

  std::string out_dir = out_dir_arg;
  if (const char* env_dir = std::getenv("AIBOX_WEBRTC_PAD_DUMP_DIR")) {
    if (env_dir[0]) out_dir = env_dir;
  }
  if (out_dir.empty()) out_dir = "./output/webrtc_pad_dump";
  MkDirP("./output");
  MkDirP(out_dir);

  const int skip = EnvInt("AIBOX_WEBRTC_PAD_DUMP_SKIP", 40);
  const int count = EnvInt("AIBOX_WEBRTC_PAD_DUMP_COUNT", 3);
  // AIBOX_WEBRTC_PAD_DUMP_ONLY=encoded → only P5/P5b (no raw CPU map).
  // Avoids DMA-sync confounder from gst_video_frame_map on P1–P4.
  const char* only = std::getenv("AIBOX_WEBRTC_PAD_DUMP_ONLY");
  const bool encoded_only =
      only && (std::strcmp(only, "encoded") == 0 ||
               std::strcmp(only, "P5b") == 0 || std::strcmp(only, "p5b") == 0);

  std::fprintf(stderr,
               "[pad-dump] enabled out_dir=%s skip=%d count=%d only=%s\n",
               out_dir.c_str(), skip, count, encoded_only ? "encoded" : "all");

  GstElement* caps = nullptr;
  GstElement* enc = nullptr;
  if (!reg.webrtc_encoder_by_camera.empty()) {
    auto it = reg.webrtc_encoder_by_camera.begin();
    enc = it->second ? GST_ELEMENT(gst_object_ref(it->second)) : nullptr;
    std::string cam = it->first;
    std::string caps_name = "caps_webrtc_" + cam;
    caps = FindByName(pipeline, caps_name.c_str());
  }
  if (!caps) caps = FindByPrefix(pipeline, "caps_webrtc_", false);
  if (!enc) enc = FindByPrefix(pipeline, "enc_webrtc_", false);

  if (!encoded_only) {
    GstElement* overlay = FindByPrefix(pipeline, "overlay_", false);
    Attach(pipeline, "P1", overlay, "src", false, out_dir, skip, count);

    GstElement* demux_w = FindByPrefix(pipeline, "demux_webrtc_", false);
    Attach(pipeline, "P2", demux_w, "src_0", false, out_dir, skip, count);

    GstElement* demux_m = FindByPrefix(pipeline, "demux_", true);
    Attach(pipeline, "P2m", demux_m, "src_0", false, out_dir, skip, count);

    Attach(pipeline, "P3", caps, "src", false, out_dir, skip, count);
    Attach(pipeline, "P4", enc ? GST_ELEMENT(gst_object_ref(enc)) : nullptr,
           "sink", false, out_dir, skip, count);
  } else if (caps) {
    gst_object_unref(caps);
    caps = nullptr;
  }

  GstPad* enc_src = nullptr;
  if (enc) enc_src = gst_element_get_static_pad(enc, "src");

  // P5/P5b: wait for SPS/IDR; upstream force-key-unit via encoder SRC pad
  Attach(pipeline, "P5",
         enc ? GST_ELEMENT(gst_object_ref(enc)) : nullptr, "src", true, out_dir,
         skip, count, true, enc_src);

  GstElement* parse = nullptr;
  if (!reg.webrtc_h264parse_by_camera.empty()) {
    auto it = reg.webrtc_h264parse_by_camera.begin();
    if (it->second) parse = GST_ELEMENT(gst_object_ref(it->second));
  }
  if (!parse) parse = FindByPrefix(pipeline, "parse_webrtc_", false);
  Attach(pipeline, "P5b", parse, "src", true, out_dir, skip, count, true,
         enc_src);

  if (enc_src) gst_object_unref(enc_src);
  if (enc) gst_object_unref(enc);

  WriteSummary(out_dir);
}

void UninstallWebrtcPadDumps() {
  if (!g_installed.exchange(false)) return;

  // Swap probes out under lock, then release before remove_probe / I/O.
  // Previous code held g_mu across WriteSummary() which also locked g_mu
  // (non-recursive) → self-deadlock on SIGTERM after E0 pad-dump.
  std::vector<ProbeCtx*> probes;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    probes.swap(g_probes);
  }

  std::string out_dir;
  for (auto* ctx : probes) {
    if (!ctx) continue;
    if (out_dir.empty()) out_dir = ctx->out_dir;
    if (ctx->pad && ctx->probe_handle) {
      gst_pad_remove_probe(ctx->pad, ctx->probe_handle);
      ctx->probe_handle = 0;
    }
    if (ctx->pad) {
      gst_object_unref(ctx->pad);
      ctx->pad = nullptr;
    }
    if (ctx->enc_src_for_ku) {
      gst_object_unref(ctx->enc_src_for_ku);
      ctx->enc_src_for_ku = nullptr;
    }
  }
  if (!out_dir.empty())
    WriteSummaryFrom(out_dir, probes);  // final counts, once
  for (auto* ctx : probes) delete ctx;
  std::fprintf(stderr, "[pad-dump] uninstalled\n");
}

}  // namespace aibox::webrtc
