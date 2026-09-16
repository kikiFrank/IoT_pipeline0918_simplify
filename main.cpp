#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include <climits>
#include <sys/resource.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include "aibox/config/TxtSnapshotLoader.hpp"
#include "aibox/orchestrator/PipelineOrchestrator.hpp"
#include "aibox/orchestrator/PlanExporter.hpp"
#include "aibox/pipeline/PipelineStructureDumper.hpp"
#include "aibox/pipeline/GstPipelineBuilder.hpp"
#include "aibox/pipeline/GstDotDumper.hpp"
#include "aibox/pipeline/AppSinkCallbacks.hpp"
#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/publisher/JsonFileEventSink.hpp"
#include "aibox/publisher/DetectionEventPublisher.hpp"

#if defined(HAVE_WEBRTC) || defined(HAVE_DETECTION_NATS)
#include "aibox/adapter/NatsAdapter.hpp"
#endif
#ifdef HAVE_WEBRTC
#include "aibox/webrtc/SessionManager.hpp"
#include "aibox/webrtc/WebrtcPadDump.hpp"
#endif
#ifdef HAVE_DETECTION_NATS
#include "aibox/detection/CaptureTimestampTracker.hpp"
#include "aibox/detection/FrameEventPublisher.hpp"
#include "aibox/detection/NatsFrameTransport.hpp"
#include "aibox/media/AnomalySnapshotService.hpp"
#include "aibox/media/MediaFileWriter.hpp"
#include "aibox/media/NatsMediaReadyTransport.hpp"
#include "aibox/media/SnapshotEncoder.hpp"
#include "aibox/media/ThumbnailEncoder.hpp"
#endif

// ===================================================================
// Argument parsing (simple, no external library)
// ===================================================================
struct Args {
  std::string snapshot_path;
  bool        dump_plan_only = false;
  bool        build_only     = false;
  int         run_seconds    = 0;   // 0 = don't run
  bool        run_forever    = false;
  bool        help           = false;
  // Phase 5: mosaic recording
  bool        record         = false;
  std::string record_path    = "/tmp/mosaic.mkv";
  int         tile_w         = 320;
  int         tile_h         = 240;
  int         mosaic_cols    = 4;
  std::string media_dir;
};

static void PrintBanner() {
  std::cout << "============================================================" << std::endl;
  std::cout << "  AIBox Stream Pipeline Demo  v1.0.0" << std::endl;
  std::cout << "============================================================" << std::endl;
}

static void PrintHelp(const char* prog) {
  PrintBanner();
  std::cout << std::endl;
  std::cout << "Usage: " << prog << " [options]" << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  --help                    Show this help" << std::endl;
  std::cout << "  --snapshot <path>         Path to pipeline.txt snapshot" << std::endl;
  std::cout << "  --dump-plan-only          Stop after exporting execution_plan.json" << std::endl;
  std::cout << "                            and pipeline_structure.txt" << std::endl;
  std::cout << "  --build-only              Build GstPipeline and export dot, then exit" << std::endl;
  std::cout << "  --run-seconds <N>         Run pipeline for N seconds, then stop" << std::endl;
  std::cout << "  --run-forever             Run until SIGINT/SIGTERM (no wall-clock deadline)" << std::endl;
  std::cout << "  --record [path]           Enable mosaic MKV recording (default: /tmp/mosaic.mkv)" << std::endl;
  std::cout << "  --tile-w <W>              Per-camera tile width  (default: 320)" << std::endl;
  std::cout << "  --tile-h <H>              Per-camera tile height (default: 240)" << std::endl;
  std::cout << "  --mosaic-cols <N>         Grid columns (default: 4 → 4x4 for 16 cams)" << std::endl;
  std::cout << "  --media-dir <path>        JPEG snapshot directory (default: $AIBOX_MEDIA_DIR or ./media)" << std::endl;
  std::cout << std::endl;
  std::cout << "Examples:" << std::endl;
  std::cout << "  " << prog << " --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt --dump-plan-only" << std::endl;
  std::cout << "  " << prog << " --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt --build-only" << std::endl;
  std::cout << "  " << prog << " --snapshot ./config_snapshots/demo_2stage_yolo_serial/pipeline.txt --run-seconds 20" << std::endl;
}

static Args ParseArgs(int argc, char** argv) {
  Args args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help") {
      args.help = true;
      return args;
    } else if (arg == "--snapshot" && i + 1 < argc) {
      args.snapshot_path = argv[++i];
    } else if (arg == "--dump-plan-only") {
      args.dump_plan_only = true;
    } else if (arg == "--build-only") {
      args.build_only = true;
    } else if (arg == "--run-seconds" && i + 1 < argc) {
      args.run_seconds = std::atoi(argv[++i]);
      if (args.run_seconds <= 0) {
        std::cerr << "[WARN] --run-seconds must be > 0, ignoring" << std::endl;
        args.run_seconds = 0;
      }
    } else if (arg == "--run-forever") {
      args.run_forever = true;
    } else if (arg == "--record") {
      args.record = true;
      // optional path follows if it doesn't start with '--'
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        args.record_path = argv[++i];
      }
    } else if (arg == "--tile-w" && i + 1 < argc) {
      args.tile_w = std::atoi(argv[++i]);
    } else if (arg == "--tile-h" && i + 1 < argc) {
      args.tile_h = std::atoi(argv[++i]);
    } else if (arg == "--mosaic-cols" && i + 1 < argc) {
      args.mosaic_cols = std::atoi(argv[++i]);
    } else if (arg == "--media-dir" && i + 1 < argc) {
      args.media_dir = argv[++i];
    } else {
      std::cerr << "[WARN] Unknown argument: " << arg << std::endl;
    }
  }

  return args;
}

// ===================================================================
// 路径解析 — 让程序可从任意目录启动（不再强制 cd build/）。
//   配置中的 model_path / labels_path 写作 "../data/models/..."，
//   是相对于可执行文件所在目录(build/)的。这里以可执行文件目录为锚点
//   把它们解析成绝对路径, 从而不依赖当前工作目录。
// ===================================================================
static std::string GetExecutableDir() {
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) return ".";
  buf[len] = '\0';
  std::string p(buf);
  size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

#ifdef HAVE_DETECTION_NATS
static std::string ResolveMediaDir(const std::string& arg_dir) {
  std::string dir = arg_dir;
  if (dir.empty()) {
    if (const char* env = std::getenv("AIBOX_MEDIA_DIR")) {
      if (env[0] != '\0') dir = env;
    }
  }
  if (dir.empty()) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
      dir = std::string(cwd) + "/media";
    } else {
      dir = "./media";
    }
  }
  char resolved[PATH_MAX];
  if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
  return dir;
}
#endif

// 若 path 为相对路径, 以 base 为锚点解析为绝对路径; 已是绝对路径则原样返回。
static std::string ResolveAgainstDir(const std::string& base,
                                     const std::string& path) {
  if (path.empty() || path[0] == '/') return path;
  std::string joined = base + "/" + path;
  char resolved[PATH_MAX];
  if (realpath(joined.c_str(), resolved) != nullptr)
    return std::string(resolved);
  return joined;  // 文件暂不存在时退回拼接路径(仍为绝对形式)
}

// ===================================================================
// Graceful shutdown — 关键：进程被 Ctrl+C / kill(SIGTERM) 时，
// 必须把 pipeline 置为 GST_STATE_NULL，否则 v4l2h264dec 持有的
// VPU (msm_vidc) 硬件解码会话不会释放，累积到 32 上限后会导致
// 后续 open('/dev/video32') 返回 ENOMEM，v4l2h264dec 无法再注册。
// ===================================================================
static volatile sig_atomic_t g_interrupted = 0;
static void HandleSignal(int sig) {
  (void)sig;
  g_interrupted = 1;
}

static void PrintFrameCountSummary() {
  auto& snaps = aibox::pipeline::frame_count_snapshots;
  if (snaps.empty()) return;
  std::ofstream fc_out("./output/frame_count_summary.txt");
  const int CW = 10;
  guint64 tot_in = 0, tot_out = 0;
  auto emit = [&](const std::string& label, guint64 v1, guint64 v2, guint64 v3) {
    std::cout << std::left  << std::setw(10) << label
              << std::right << std::setw(CW) << v1
              << std::setw(CW) << v2
              << std::setw(CW) << v3 << std::endl;
    if (fc_out.is_open())
      fc_out << std::left  << std::setw(10) << label
             << std::right << std::setw(CW) << v1
             << std::setw(CW) << v2
             << std::setw(CW) << v3 << "\n";
  };
  std::cout << std::endl;
  std::cout << "=== Frame Count Summary ===" << std::endl;
  if (fc_out.is_open())
    fc_out << "=== Frame Count Summary ===\n";
  std::cout << std::left  << std::setw(10) << "Camera"
            << std::right << std::setw(CW) << "IN"
            << std::setw(CW) << "OUT"
            << std::setw(CW) << "delta" << std::endl;
  if (fc_out.is_open())
    fc_out << std::left  << std::setw(10) << "Camera"
           << std::right << std::setw(CW) << "IN"
           << std::setw(CW) << "OUT"
           << std::setw(CW) << "delta\n";
  for (auto& s : snaps) {
    guint64 d = (s.frames_in >= s.frames_out) ? (s.frames_in - s.frames_out) : 0;
    emit(s.camera_id, s.frames_in, s.frames_out, d);
    tot_in  += s.frames_in;
    tot_out += s.frames_out;
  }
  std::string sep(10 + 3 * CW, '-');
  std::cout << sep << std::endl;
  if (fc_out.is_open()) fc_out << sep << "\n";
  emit("TOTAL", tot_in, tot_out, (tot_in >= tot_out) ? (tot_in - tot_out) : 0);
}

#ifdef HAVE_WEBRTC
// Phase C: runner/systemd maps these to Restart=on-failure.
// 0 = clean (signal or --run-seconds deadline). Non-zero = supervised retry.
enum class RunExitReason : int {
  kOk = 0,
  kCircuitBreaker = 2,
  kSoftRtsp = 3,
  kBusError = 4,
  kEos = 5,
};

// CP04: run WebRTC control-plane work from the default GLib main context.
// GStreamer streaming threads still emit their own signals, but all queued
// session mutations are drained only by this source while the context is owned.
struct WebrtcRunLoopContext {
  GMainLoop* loop = nullptr;
  GstElement* pipeline = nullptr;
  aibox::webrtc::SessionManager* session_mgr = nullptr;
  gint64 deadline_us = 0;  // 0 = --run-forever (no wall-clock stop)
  bool tick_active = true;
  bool terminal_bus_message = false;
  RunExitReason exit_reason = RunExitReason::kOk;
  // CP07-rtsp: soft RTSP bus errors are source-scoped and bounded.
  // Exceeding the budget exits so the runner can restart the whole pipeline
  // (no in-process source hot-rebuild; network faults must not reset VPU).
  guint soft_rtsp_errors = 0;
  guint soft_rtsp_budget = 3;
  gint64 first_soft_rtsp_mono_us = 0;
  gint64 last_soft_rtsp_mono_us = 0;
};

static gboolean WebrtcRunTick(gpointer user_data) {
  auto* ctx = static_cast<WebrtcRunLoopContext*>(user_data);
  if (g_interrupted) {
    ctx->tick_active = false;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
  }
  if (ctx->deadline_us > 0 && g_get_monotonic_time() >= ctx->deadline_us) {
    ctx->tick_active = false;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
  }
  if (ctx->session_mgr) {
    // Per-cam offer: always drain session queues. DoInit is capped at 1/tick
    // inside ProcessAllQueues so a serial Venus first-frame wait is not
    // starved, and READY cameras are not blocked behind another BUILDING cam.
    ctx->session_mgr->ProcessAllQueues();
    ctx->session_mgr->TickWatchdog();
    // CP05-R1A: encoder stall circuit-breaker → orderly exit for runner.
    if (ctx->session_mgr->ConsumeCircuitBreakerTrip()) {
      std::cerr << "[ERROR] Encoder circuit-breaker tripped; quitting main "
                   "loop for runner retry (no VPU reset)\n";
      ctx->terminal_bus_message = true;
      ctx->exit_reason = RunExitReason::kCircuitBreaker;
      ctx->tick_active = false;
      g_main_loop_quit(ctx->loop);
      return G_SOURCE_REMOVE;
    }
  }
  return G_SOURCE_CONTINUE;
}

static gboolean WebrtcRunBusWatch(GstBus* /*bus*/, GstMessage* msg,
                                  gpointer user_data) {
  auto* ctx = static_cast<WebrtcRunLoopContext*>(user_data);
  const GstMessageType type = GST_MESSAGE_TYPE(msg);
  const gchar* src_name = GST_MESSAGE_SRC(msg)
      ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : nullptr;

  if (type == GST_MESSAGE_EOS) {
    // One camera EOS must not tear down the remaining live cameras / NATS
    // signaling. Pipeline-level EOS (src name == pipeline) still stops.
    if (src_name && std::strncmp(src_name, "rtspsrc", 7) == 0) {
      std::cerr << "[WARN] ignore EOS from " << src_name
                << " — keep pipeline for remaining cameras" << std::endl;
      return G_SOURCE_CONTINUE;
    }
    ctx->terminal_bus_message = true;
    ctx->exit_reason = RunExitReason::kEos;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_CONTINUE;
  }

  if (type == GST_MESSAGE_CLOCK_LOST) {
    std::cerr << "[WARN] CLOCK_LOST src=" << (src_name ? src_name : "?")
              << " — pipeline PAUSED→PLAYING to reselect clock" << std::endl;
    if (ctx->pipeline) {
      gst_element_set_state(ctx->pipeline, GST_STATE_PAUSED);
      gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING);
    }
    return G_SOURCE_CONTINUE;
  }

  if (type == GST_MESSAGE_WARNING) {
    GError* err = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_warning(msg, &err, &dbg);
    std::cerr << "[WARN] bus src=" << (src_name ? src_name : "?")
              << " " << ((err && err->message) ? err->message : "?")
              << std::endl;
    if (dbg) std::cerr << "[DEBUG] " << dbg << std::endl;
    g_free(dbg);
    if (err) g_error_free(err);
    return G_SOURCE_CONTINUE;
  }

  if (type == GST_MESSAGE_ELEMENT) {
    const GstStructure* s = gst_message_get_structure(msg);
    std::cerr << "[INFO] ELEMENT src=" << (src_name ? src_name : "?")
              << " name=" << (s ? gst_structure_get_name(s) : "?")
              << std::endl;
    return G_SOURCE_CONTINUE;
  }

  if (type != GST_MESSAGE_ERROR) return G_SOURCE_CONTINUE;

  GError* err = nullptr;
  gchar* dbg = nullptr;
  gst_message_parse_error(msg, &err, &dbg);
  const char* err_msg = (err && err->message) ? err->message : "";
  std::cerr << "[ERROR] " << (err_msg[0] ? err_msg : "?") << std::endl;
  if (dbg) std::cerr << "[DEBUG] " << dbg << std::endl;

  const bool from_rtspsrc =
      (src_name && std::strncmp(src_name, "rtspsrc", 7) == 0);
  const bool rtsp_transport_msg =
      (err_msg[0] &&
       (std::strstr(err_msg, "Could not read from resource") ||
        std::strstr(err_msg, "Could not receive message") ||
        std::strstr(err_msg, "Parse error") ||
        std::strstr(err_msg, "streaming stopped")));
  const bool soft_rtsp = from_rtspsrc || rtsp_transport_msg;
  const bool vpu_enomem =
      err_msg[0] &&
      std::strstr(err_msg, "Cannot allocate memory") &&
      (std::strstr(err_msg, "/dev/video") ||
       (src_name && std::strstr(src_name, "v4l2")));

  if (vpu_enomem) {
    std::cerr << "[WARN] VPU ENOMEM src=" << (src_name ? src_name : "?")
              << " — not fatal; runner owns Venus session recovery"
              << std::endl;
    g_free(dbg);
    g_error_free(err);
    return G_SOURCE_CONTINUE;
  }

  if (soft_rtsp) {
    const gint64 now = g_get_monotonic_time();
    constexpr gint64 kSoftRtspDecayUs = 120 * G_TIME_SPAN_SECOND;
    if (ctx->last_soft_rtsp_mono_us > 0 &&
        (now - ctx->last_soft_rtsp_mono_us) > kSoftRtspDecayUs) {
      ctx->soft_rtsp_errors = 0;
    }
    if (ctx->first_soft_rtsp_mono_us == 0) ctx->first_soft_rtsp_mono_us = now;
    ctx->last_soft_rtsp_mono_us = now;
    ++ctx->soft_rtsp_errors;
    std::cerr << "[WARN] Soft RTSP bus ERROR src="
              << (src_name ? src_name : "?")
              << " count=" << ctx->soft_rtsp_errors
              << "/" << ctx->soft_rtsp_budget
              << " first_mono_us=" << ctx->first_soft_rtsp_mono_us
              << " now_mono_us=" << now
              << " msg=" << (err_msg[0] ? err_msg : "?")
              << " — no in-process source rebuild; runner owns restart"
              << std::endl;
    g_free(dbg);
    g_error_free(err);
    if (ctx->soft_rtsp_errors >= ctx->soft_rtsp_budget) {
      std::cerr << "[ERROR] Soft RTSP budget exhausted ("
                << ctx->soft_rtsp_budget
                << "); exiting for runner retry (no VPU reset)"
                << std::endl;
      ctx->terminal_bus_message = true;
      ctx->exit_reason = RunExitReason::kSoftRtsp;
      g_main_loop_quit(ctx->loop);
    }
    return G_SOURCE_CONTINUE;
  }

  g_free(dbg);
  g_error_free(err);
  ctx->terminal_bus_message = true;
  ctx->exit_reason = RunExitReason::kBusError;
  g_main_loop_quit(ctx->loop);
  return G_SOURCE_CONTINUE;
}
#endif

/* Request pads obtained by the builder carry an owned reference.  Releasing
 * them explicitly, while the pipeline still owns the splitter and its children,
 * keeps teardown independent of GstElement's fallback dispose ordering. */
static void ReleaseSplitterRequestPads(
    aibox::pipeline::RuntimeRegistry& registry) {
  const size_t count = registry.webrtc_splitter_src_by_camera.size();
  if (count == 0) return;

  std::cout << "[INFO] Releasing " << count
            << " owned splitter request pad(s)..." << std::endl;
  for (auto& kv : registry.webrtc_splitter_src_by_camera) {
    GstPad* pad = kv.second;
    if (!pad) continue;

    GstPad* peer = gst_pad_get_peer(pad);
    if (peer) {
      gst_pad_unlink(pad, peer);
      gst_object_unref(peer);
    }
    GstObject* parent = gst_object_get_parent(GST_OBJECT(pad));
    if (parent && GST_IS_ELEMENT(parent))
      gst_element_release_request_pad(GST_ELEMENT(parent), pad);
    else
      std::cerr << "[WARN] splitter request pad for " << kv.first
                << " has no live element parent" << std::endl;
    if (parent) gst_object_unref(parent);
    gst_object_unref(pad);
  }
  registry.webrtc_splitter_src_by_camera.clear();
}

static void PrintBatchRuntimeStats(
    const aibox::pipeline::RuntimeRegistry& registry) {
  if (registry.batch_group_runtime.empty()) return;
  guint64 batch_total = 0;
  guint64 batch_partial = 0;
  for (const auto& kv : registry.batch_group_runtime) {
    if (kv.second.batch_total) batch_total += kv.second.batch_total->load();
    if (kv.second.batch_partial)
      batch_partial += kv.second.batch_partial->load();
  }
  std::cout << "[BATCH_STATS] batch_total=" << batch_total
            << " batch_partial=" << batch_partial
            << std::endl;
  for (const auto& kv : registry.batch_group_runtime) {
    GstElement* splits[2] = {kv.second.batch_split, kv.second.batch_split_s2};
    const char* tags[2] = {"s1", "s2"};
    for (int i = 0; i < 2; ++i) {
      GstElement* split = splits[i];
      if (!split) continue;
      guint64 forwarded = 0, drop_part = 0, invalid = 0, input = 0;
      g_object_get(split,
                   "forwarded", &forwarded,
                   "drop-partial", &drop_part,
                   "invalid-batch", &invalid,
                   "input-frames", &input,
                   nullptr);
      std::cout << "[BATCH_SPLIT] group=" << kv.first
                << " stage=" << tags[i]
                << " forwarded=" << forwarded
                << " drop_partial=" << drop_part
                << " invalid_batch=" << invalid
                << " input_frames=" << input
                << std::endl;
    }
    if (kv.second.lockstep) {
      std::cout << "[LOCKSTEP] group=" << kv.first
                << " pairs=" << kv.second.lockstep->pairs.load()
                << std::endl;
    }
  }
}

static void ReleaseBatchRuntime(
    aibox::pipeline::RuntimeRegistry& registry) {
  aibox::pipeline::DetachBatchQnnProbes(registry);
  if (!registry.request_pad_handles.empty()) {
    std::cout << "[INFO] Releasing " << registry.request_pad_handles.size()
              << " batch request pad(s)..." << std::endl;
  }
  aibox::pipeline::ReleaseAllRequestPads(registry);
}

// Raise RLIMIT_NOFILE early: 9x webrtcbin + pipeline baseline exceeds
// the default soft limit (1024) and aborts with GLib "Too many open files".
static void RaiseNofileLimit() {
  constexpr rlim_t kWant = 65536;
  struct rlimit rl {};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    std::cerr << "[WARN] getrlimit(RLIMIT_NOFILE) failed: "
              << std::strerror(errno) << std::endl;
    return;
  }
  if (rl.rlim_cur < kWant) {
    struct rlimit want = rl;
    want.rlim_cur = kWant;
    if (want.rlim_max < kWant && want.rlim_max != RLIM_INFINITY) {
      want.rlim_max = kWant;
    }
    if (setrlimit(RLIMIT_NOFILE, &want) != 0) {
      // Fall back: raise soft to current hard (no privilege needed).
      want = rl;
      if (want.rlim_max != RLIM_INFINITY && want.rlim_max > want.rlim_cur) {
        want.rlim_cur = want.rlim_max;
      } else if (want.rlim_max == RLIM_INFINITY) {
        want.rlim_cur = kWant;
      }
      if (setrlimit(RLIMIT_NOFILE, &want) != 0) {
        std::cerr << "[WARN] setrlimit(RLIMIT_NOFILE) failed: "
                  << std::strerror(errno) << std::endl;
      }
    }
  }
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    std::cout << "[INFO] RLIMIT_NOFILE soft=" << rl.rlim_cur
              << " hard=" << rl.rlim_max << std::endl;
  }
}

// ===================================================================
// main
// ===================================================================
int main(int argc, char** argv) {
  Args args = ParseArgs(argc, argv);

  if (args.help) {
    PrintHelp(argv[0]);
    return 0;
  }

  PrintBanner();
  RaiseNofileLimit();

  // --- GStreamer init ---
  gst_init(&argc, &argv);
  std::cout << "[INFO] GStreamer initialized (version "
            << GST_VERSION_MAJOR << "."
            << GST_VERSION_MINOR << "."
            << GST_VERSION_MICRO << ")" << std::endl;

  // --- validate arguments ---
  if (args.snapshot_path.empty()) {
    std::cerr << "[ERROR] --snapshot is required. Use --help for usage." << std::endl;
    return 1;
  }

  std::cout << "[INFO] snapshot : " << args.snapshot_path << std::endl;
  std::cout << "[INFO] dump_plan_only : " << (args.dump_plan_only ? "yes" : "no") << std::endl;
  std::cout << "[INFO] build_only     : " << (args.build_only ? "yes" : "no") << std::endl;
  std::cout << "[INFO] run_seconds    : " << args.run_seconds << std::endl;
  std::cout << "[INFO] run_forever    : " << (args.run_forever ? "yes" : "no") << std::endl;

  // --- Step 2: TxtSnapshotLoader ---
  aibox::pipeline::TxtSnapshotLoader loader;
  auto config_result = loader.Load(args.snapshot_path);
  if (!config_result.ok()) {
    std::cerr << "[ERROR] Failed to load snapshot: "
              << config_result.status.message << std::endl;
    return 1;
  }
  const auto& config = config_result.value;
  std::cout << "[INFO] Snapshot loaded: " << config.global.demo_name << std::endl;
  std::cout << "[INFO]   streams   : " << config.streams.size() << std::endl;
  std::cout << "[INFO]   algorithms: " << config.algorithms.size() << std::endl;
  std::cout << "[INFO]   models    : " << config.models.size() << std::endl;
  // --- Step 3: PipelineOrchestrator ---
  aibox::pipeline::PipelineOrchestrator orchestrator;
  auto plan_result = orchestrator.BuildExecutionPlan(config);
  if (!plan_result.ok()) {
    std::cerr << "[ERROR] Orchestrator failed: "
              << plan_result.status.message << std::endl;
    return 1;
  }
  auto& plan = plan_result.value;

  // 以可执行文件目录(build/)为锚点, 把各 stage 的模型/标签相对路径解析成
  // 绝对路径。这样无论从哪个目录启动程序都能正确加载, 消除"必须从 build/ 运行"。
  {
    const std::string exe_dir = GetExecutableDir();
    for (auto& sp : plan.stage_plans) {
      sp.model_path  = ResolveAgainstDir(exe_dir, sp.model_path);
      sp.labels_path = ResolveAgainstDir(exe_dir, sp.labels_path);
    }
    if (plan.face_plan.enabled) {
      plan.face_plan.model_path  = ResolveAgainstDir(exe_dir, plan.face_plan.model_path);
      plan.face_plan.config_path = ResolveAgainstDir(exe_dir, plan.face_plan.config_path);
    }
  }

  std::cout << "[INFO] ExecutionPlan  : " << plan.plan_id << std::endl;
  std::cout << "[INFO]   stream_plans : " << plan.stream_plans.size() << std::endl;
  std::cout << "[INFO]   algo_groups  : " << plan.algorithm_group_plans.size() << std::endl;
  std::cout << "[INFO]   time_mux_grps: " << plan.time_mux_group_plans.size() << std::endl;
  std::cout << "[INFO]   stage_plans  : " << plan.stage_plans.size() << std::endl;
  std::cout << "[INFO]   chain_plans  : " << plan.inference_chain_plans.size() << std::endl;
  // --- Step 4: PlanExporter → execution_plan.json ---
  aibox::pipeline::PlanExporter plan_exporter;
  aibox::pipeline::Status st = plan_exporter.ExportExecutionPlanJson(
      plan, plan.gst_pipeline_plan.execution_plan_path);
  if (!st.ok) {
    std::cerr << "[ERROR] PlanExporter failed: " << st.message << std::endl;
    return 1;
  }
  std::cout << "[INFO] Exported: " << plan.gst_pipeline_plan.execution_plan_path << std::endl;
  // --- Step 5: PipelineStructureDumper → pipeline_structure.txt ---
  aibox::pipeline::PipelineStructureDumper structure_dumper;
  st = structure_dumper.Dump(plan, plan.gst_pipeline_plan.structure_txt_path);
  if (!st.ok) {
    std::cerr << "[ERROR] StructureDumper failed: " << st.message << std::endl;
    return 1;
  }
  std::cout << "[INFO] Exported: " << plan.gst_pipeline_plan.structure_txt_path << std::endl;
  // --- dump-plan-only early exit ---
  if (args.dump_plan_only) {
    std::cout << "[INFO] --dump-plan-only: stopping after plan export." << std::endl;
    return 0;
  }

  // --- Step 6: GstPipelineBuilder ---
  aibox::pipeline::GstPipelineBuildOptions build_opts;
  build_opts.enable_yolo_stages = true;  // P2/P3: YOLO stages enabled

  // WebRTC: enable if configured in [webrtc] section
  if (plan.webrtc_config.enabled) {
    build_opts.enable_webrtc = true;
    std::cout << "[INFO] WebRTC annotated preview enabled" << std::endl;
  }

  // Phase 5: mosaic recording
  if (args.record) {
    build_opts.enable_mosaic_recording = true;
    build_opts.mosaic_output_path      = args.record_path;
    build_opts.tile_w                  = args.tile_w;
    build_opts.tile_h                  = args.tile_h;
    build_opts.mosaic_cols             = args.mosaic_cols;
    std::cout << "[INFO] Mosaic recording enabled → " << args.record_path
              << "  tile=" << args.tile_w << "x" << args.tile_h
              << "  cols=" << args.mosaic_cols << std::endl;
  }
  aibox::pipeline::GstPipelineBuilder builder(build_opts);
  auto build_result = builder.Build(plan);
  if (!build_result.ok()) {
    std::cerr << "[ERROR] PipelineBuilder failed: "
              << build_result.status.message << std::endl;
    return 1;
  }
  std::cout << "[INFO] GstPipeline built successfully (P1: mux + decode + fakesink)" << std::endl;

  // --- WebRTC SessionManager (Phase 3+4) ---
#ifdef HAVE_WEBRTC
  std::unique_ptr<aibox::adapter::NatsAdapter> g_nats_adapter;
  std::unique_ptr<aibox::webrtc::SessionManager> g_session_mgr;
  bool nats_library_opened = false;
  auto ShutdownWebrtcAndNats = [&] {
    aibox::webrtc::UninstallWebrtcPadDumps();
    if (g_session_mgr) {
      g_session_mgr->Shutdown();
      g_session_mgr.reset();
    }
    if (g_nats_adapter) {
      g_nats_adapter->Disconnect();
      g_nats_adapter.reset();
    }
    if (nats_library_opened) {
      if (!aibox::adapter::NatsAdapter::CloseLibraryAndWait()) {
        std::cerr << "[WARN] NATS internal threads did not stop cleanly"
                  << std::endl;
      }
      nats_library_opened = false;
    }
  };

  // Diagnostic pad dump (AIBOX_WEBRTC_PAD_DUMP=1); default OFF.
  if (build_opts.enable_webrtc) {
    aibox::webrtc::MaybeInstallWebrtcPadDumps(
        build_result.value.pipeline, build_result.value.registry,
        "./output/webrtc_pad_dump");
  }

  if (build_opts.enable_webrtc) {
    // Init NATS adapter
    g_nats_adapter.reset(new aibox::adapter::NatsAdapter());
    aibox::adapter::NatsAdapter::Config nats_cfg;
    nats_cfg.nats_url    = "nats://localhost:4222";
    nats_cfg.client_name = "aibox-pipeline-webrtc";
    nats_library_opened = true;
    if (g_nats_adapter->Connect(nats_cfg)) {
      // Init SessionManager
      g_session_mgr.reset(new aibox::webrtc::SessionManager());
      auto& reg = build_result.value.registry;
      g_session_mgr->Init(build_result.value.pipeline, g_nats_adapter.get(),
                          &reg);

      // Register cameras: ready tees and/or lazy stubs
      for (auto& kv : reg.webrtc_tee_by_camera) {
        g_session_mgr->RegisterCamera(kv.first, kv.second);
      }
      for (auto& kv : reg.webrtc_stub_queue_by_camera) {
        if (reg.webrtc_tee_by_camera.count(kv.first) == 0)
          g_session_mgr->RegisterCamera(kv.first, nullptr);
      }

      // Subscribe NATS wildcards before PLAYING
      if (!g_session_mgr->StartSignaling()) {
        std::cerr << "[WARN] WebRTC signaling start failed — sessions disabled"
                  << std::endl;
        ShutdownWebrtcAndNats();
      }
    } else {
      std::cerr << "[WARN] NATS connect failed — WebRTC sessions disabled"
                << std::endl;
      ShutdownWebrtcAndNats();
    }
  }
#endif

  // --- Result aggregator: receives fused per-frame results from the terminal
  //     appsink and demultiplexes by camera_id (no C++ cross-stage join). ---
  static aibox::aggregator::ResultAggregator g_aggregator(
      aibox::aggregator::ResultAggregator::Options{});
#ifdef HAVE_DETECTION_NATS
  aibox::detection::CaptureTimestampTracker capture_tracker;
  aibox::detection::CaptureTimestampTracker::SetProcessInstance(&capture_tracker);
#endif

  // --- Register appsink callbacks ---
  auto& appsinks = build_result.value.registry.appsinks;
  std::cout << "[INFO] Registering " << appsinks.size()
            << " appsink callbacks..." << std::endl;
  for (auto& entry : appsinks) {
    // Chained-metamux topology registers exactly one terminal appsink per
    // TimeMuxGroup, carrying a fused VIDEO buffer (all stages' ROI meta).
    // Parse it into an AggregatedResult and feed the ResultAggregator.
    GstAppSinkCallbacks cbs = {};
    cbs.new_sample = aibox::pipeline::OnTerminalSampleToAggregator;
    g_object_set_data_full(G_OBJECT(entry.appsink), "stage-id",
                           g_strdup(entry.stage_id.c_str()), g_free);
    gst_app_sink_set_callbacks(GST_APP_SINK(entry.appsink), &cbs,
                               &g_aggregator, nullptr);
    std::cout << "[INFO]   appsink registered: stage=" << entry.stage_id
              << " (terminal/metamux -> aggregator)" << std::endl;
  }

  // --- Step 9: GstDotDumper + auto-render ---
  aibox::pipeline::GstDotDumper dot_dumper;
  aibox::pipeline::Status dot_st = dot_dumper.DumpDot(
      build_result.value.pipeline, plan.gst_pipeline_plan.runtime_dot_path);
  if (!dot_st.ok)
    std::cerr << "[WARN] Dot export failed: " << dot_st.message << std::endl;
  else {
    std::cout << "[INFO] Exported: " << plan.gst_pipeline_plan.runtime_dot_path << std::endl;
    // Auto-render svg + png
    std::string cmd = "bash scripts/render_dot.sh " +
                      plan.gst_pipeline_plan.runtime_dot_path + " " +
                      plan.gst_pipeline_plan.runtime_svg_path + " " +
                      plan.gst_pipeline_plan.runtime_png_path;
    int rc = system(cmd.c_str());
    if (rc == 0)
      std::cout << "[INFO] Rendered: " << plan.gst_pipeline_plan.runtime_svg_path
                << ", " << plan.gst_pipeline_plan.runtime_png_path << std::endl;
    else
      std::cerr << "[WARN] Dot render failed (graphviz installed?). Run: bash scripts/render_dot.sh "
                << plan.gst_pipeline_plan.runtime_dot_path << std::endl;
  }

  // --- build-only early exit ---
  if (args.build_only) {
    std::cout << "[INFO] --build-only: pipeline built, cleaning up." << std::endl;
#ifdef HAVE_WEBRTC
    ShutdownWebrtcAndNats();
#endif
    GstStateChangeReturn scr =
        gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
    if (scr == GST_STATE_CHANGE_FAILURE)
      std::cerr << "[WARN] build-only set_state(NULL) returned FAILURE" << std::endl;
    GstState st_cur = GST_STATE_VOID_PENDING;
    GstState st_pend = GST_STATE_VOID_PENDING;
    // NOTE: the 10s get_state timeout here CANNOT protect against a
    // synchronously BLOCKING gst_element_set_state(NULL) above — get_state
    // is never reached while set_state is wedged (e.g. on a pad mutex held
    // by a live dynamic chain). Reliable teardown therefore depends on
    // SessionManager::Shutdown() tearing down the dynamic encoder chains
    // BEFORE the pipeline NULL phase; an out-of-process supervisor remains
    // the final timeout guard against permanently-blocked vendor calls.
    GstStateChangeReturn gsr = gst_element_get_state(
        build_result.value.pipeline, &st_cur, &st_pend, 10 * GST_SECOND);
    if (gsr != GST_STATE_CHANGE_SUCCESS || st_cur != GST_STATE_NULL)
      std::cerr << "[WARN] build-only pipeline did not reach NULL cleanly"
                << std::endl;
    PrintBatchRuntimeStats(build_result.value.registry);
    aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);
#ifdef HAVE_DETECTION_NATS
    aibox::detection::CaptureTimestampTracker::SetProcessInstance(nullptr);
#endif
    ReleaseSplitterRequestPads(build_result.value.registry);
    ReleaseBatchRuntime(build_result.value.registry);
    gst_object_unref(build_result.value.pipeline);
    return 0;
  }

  // --- run-seconds / run-forever mode ---
  if (args.run_forever && args.run_seconds > 0) {
    std::cerr << "[WARN] both --run-forever and --run-seconds set; "
                 "using --run-forever" << std::endl;
    args.run_seconds = 0;
  }
  if (args.run_forever || args.run_seconds > 0) {
    // Install signal handlers so SIGINT/SIGTERM triggers a graceful
    // shutdown (pipeline -> NULL) and releases VPU decode sessions.
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    // SIGHUP: 覆盖"关闭终端/父进程退出"场景, 否则会跳过优雅退出,
    // pipeline 停在 PLAYING, v4l2h264dec 的 VPU 解码会话不会释放。
    std::signal(SIGHUP, HandleSignal);

    // --- Downstream publisher ---
#ifdef HAVE_DETECTION_NATS
    aibox::adapter::NatsAdapter detection_nats;
    aibox::adapter::NatsAdapter::Config detection_nats_cfg;
    detection_nats_cfg.nats_url = "nats://127.0.0.1:4222";
    if (const char* url = std::getenv("AIBOX_DETECTION_NATS_URL")) {
      if (url[0] != '\0') detection_nats_cfg.nats_url = url;
    }
    detection_nats_cfg.client_name = "aibox-pipeline-detection";
    detection_nats_cfg.max_pending = 0;
    if (!detection_nats.Connect(detection_nats_cfg)) {
      std::cerr << "[ERROR] Detection NATS connect failed: "
                << detection_nats_cfg.nats_url << std::endl;
#ifdef HAVE_WEBRTC
      ShutdownWebrtcAndNats();
#endif
      gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
      PrintBatchRuntimeStats(build_result.value.registry);
      aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);
      aibox::detection::CaptureTimestampTracker::SetProcessInstance(nullptr);
      ReleaseSplitterRequestPads(build_result.value.registry);
      ReleaseBatchRuntime(build_result.value.registry);
      gst_object_unref(build_result.value.pipeline);
      return 23;
    }
    aibox::detection::NatsFrameTransport frame_transport(detection_nats);
    aibox::detection::FrameEventPublisher publisher(g_aggregator, frame_transport);
    publisher.Start();
    std::cout << "[INFO] FrameEventPublisher started (NATS subject: detection.frame)"
              << std::endl;

    const std::string media_dir = ResolveMediaDir(args.media_dir);
    aibox::media::MediaFileWriter media_writer(media_dir);
    if (!media_writer.EnsureDirectory()) {
      std::cerr << "[WARN] media dir not writable: " << media_dir << std::endl;
    }
    aibox::media::GstSnapshotEncoder snapshot_encoder;
    aibox::media::GstThumbnailEncoder thumbnail_encoder;
    aibox::media::NatsMediaReadyTransport media_transport(detection_nats);
    aibox::media::AnomalySnapshotService snapshot_service(
        snapshot_encoder, thumbnail_encoder, media_writer, media_transport);
    aibox::media::AnomalySnapshotService::SetProcessInstance(&snapshot_service);
    snapshot_service.Start();
    std::cout << "[INFO] AnomalySnapshotService started media_dir="
              << media_writer.Directory()
              << " encoder_ready=" << (snapshot_encoder.IsReady() ? "yes" : "no")
              << " thumb_encoder_ready="
              << (thumbnail_encoder.IsReady() ? "yes" : "no")
              << std::endl;
#else
    aibox::publisher::JsonFileEventSink event_sink("./output/events.jsonl");
    aibox::publisher::DetectionEventPublisher publisher(g_aggregator, event_sink);
    publisher.Start();
    std::cout << "[INFO] DetectionEventPublisher started (sink: ./output/events.jsonl)"
              << std::endl;
#endif

    const bool noweb_fps =
        !build_opts.enable_webrtc && !args.record;
    if (noweb_fps) {
      aibox::pipeline::OpenFpsCsv("./output/fps_per_camera.csv");
      std::cout << "[INFO] FPS CSV: ./output/fps_per_camera.csv" << std::endl;
    }

    if (args.run_forever) {
      std::cout << "[INFO] Setting pipeline to PLAYING (run-forever; "
                   "Ctrl+C / SIGTERM to stop)..." << std::endl;
    } else {
      std::cout << "[INFO] Setting pipeline to PLAYING for "
                << args.run_seconds << "s (Ctrl+C to stop early)..." << std::endl;
    }

    // P0-2: verify the REAL PLAYING transition. FAILURE aborts; ASYNC /
    // NO_PREROLL must be confirmed via bus ASYNC_DONE (or pipeline-level
    // STATE_CHANGED→PLAYING) before declaring [PIPELINE_READY]. The start
    // scripts key off [PIPELINE_READY] only.
    {
      const GstStateChangeReturn pscr =
          gst_element_set_state(build_result.value.pipeline, GST_STATE_PLAYING);
      std::cout << "[INFO] set_state(PLAYING) ret=" << static_cast<int>(pscr)
                << std::endl;
      bool ready = (pscr == GST_STATE_CHANGE_SUCCESS);
      if (pscr == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[ERROR] set_state(PLAYING) FAILURE — aborting" << std::endl;
        gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
        PrintBatchRuntimeStats(build_result.value.registry);
        aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);
        gst_object_unref(build_result.value.pipeline);
        return 24;
      }
      if (pscr == GST_STATE_CHANGE_ASYNC || pscr == GST_STATE_CHANGE_NO_PREROLL) {
        GstBus* rdy_bus = gst_element_get_bus(build_result.value.pipeline);
        const gint64 rdy_deadline =
            g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
        while (!ready && g_get_monotonic_time() < rdy_deadline) {
          GstMessage* m = gst_bus_timed_pop_filtered(
              rdy_bus, GST_SECOND,
              (GstMessageType)(GST_MESSAGE_ASYNC_DONE |
                               GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR));
          if (!m) continue;
          const GstMessageType mt = GST_MESSAGE_TYPE(m);
          if (mt == GST_MESSAGE_ASYNC_DONE) {
            ready = true;
          } else if (mt == GST_MESSAGE_STATE_CHANGED) {
            GstState ns = GST_STATE_VOID_PENDING;
            gst_message_parse_state_changed(m, nullptr, &ns, nullptr);
            if (GST_MESSAGE_SRC(m) ==
                    GST_OBJECT_CAST(build_result.value.pipeline) &&
                ns == GST_STATE_PLAYING)
              ready = true;
          } else {
            GError* e = nullptr;
            gst_message_parse_error(m, &e, nullptr);
            std::cerr << "[ERROR] bring-up error: "
                      << (e && e->message ? e->message : "?") << std::endl;
            if (e) g_error_free(e);
          }
          gst_message_unref(m);
        }
        gst_object_unref(rdy_bus);
        if (!ready) {
          std::cerr << "[ERROR] pipeline did not reach PLAYING in 30s — aborting"
                    << std::endl;
          gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
          PrintBatchRuntimeStats(build_result.value.registry);
          aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);
          gst_object_unref(build_result.value.pipeline);
          return 24;
        }
      }
      std::cout << "[PIPELINE_READY] state=PLAYING" << std::endl;
    }

    GstBus* bus = gst_element_get_bus(build_result.value.pipeline);
    const gint64 deadline = args.run_forever
        ? 0
        : g_get_monotonic_time() + (gint64)args.run_seconds * G_TIME_SPAN_SECOND;

    int webrtc_exit = 0;
#ifdef HAVE_WEBRTC
    WebrtcRunLoopContext loop_ctx;
    loop_ctx.loop = g_main_loop_new(nullptr, FALSE);
    loop_ctx.pipeline = build_result.value.pipeline;
    loop_ctx.session_mgr = g_session_mgr.get();
    loop_ctx.deadline_us = deadline;
    const guint bus_watch = gst_bus_add_watch(bus, &WebrtcRunBusWatch, &loop_ctx);
    const guint tick_source = g_timeout_add(20, &WebrtcRunTick, &loop_ctx);
    g_main_loop_run(loop_ctx.loop);
    if (loop_ctx.tick_active) g_source_remove(tick_source);
    if (bus_watch) gst_bus_remove_watch(bus);
    g_main_loop_unref(loop_ctx.loop);
    webrtc_exit = static_cast<int>(loop_ctx.exit_reason);
#else
    // Non-WebRTC builds retain a small bus poll loop for --run-seconds.
    while (!g_interrupted &&
           (deadline == 0 || g_get_monotonic_time() < deadline)) {
      GstMessage* msg = gst_bus_timed_pop_filtered(
          bus, 200 * GST_MSECOND,
          (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
      if (!msg) continue;
      gst_message_unref(msg);
      break;
    }
#endif
    gst_object_unref(bus);

    int exit_code = 0;
    if (g_interrupted) {
      std::cout << "[INFO] Signal received, stopping pipeline..." << std::endl;
      exit_code = 0;
    } else if (webrtc_exit != 0) {
      exit_code = webrtc_exit;
      std::cout << "[INFO] Stopping pipeline (exit_reason=" << exit_code
                << ")..." << std::endl;
    } else {
      std::cout << "[INFO] Stopping pipeline..." << std::endl;
    }

    // --- Shutdown WebRTC sessions BEFORE pipeline teardown ---
#ifdef HAVE_WEBRTC
    std::cout << "[INFO] Shutting down WebRTC sessions..." << std::endl;
    ShutdownWebrtcAndNats();
#endif

    // Send EOS so the recording muxer/filesink can finalize the MKV before
    // we tear down the pipeline (important for matroskamux seeking table).
    if (args.record) {
      std::cout << "[INFO] Sending EOS to finalize mosaic.mkv ..." << std::endl;
      gst_element_send_event(build_result.value.pipeline, gst_event_new_eos());
      // Wait up to 5 s for EOS to propagate
      GstBus* eos_bus = gst_element_get_bus(build_result.value.pipeline);
      GstMessage* eos_msg = gst_bus_timed_pop_filtered(
          eos_bus, 5 * GST_SECOND,
          (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
      if (eos_msg) gst_message_unref(eos_msg);
      gst_object_unref(eos_bus);
    }

    PrintBatchRuntimeStats(build_result.value.registry);

    aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);

    // Snapshot IN/OUT counters while pads/probes still exist.
    aibox::pipeline::SnapshotFrameCounters(build_result.value.registry);

    // Stop the pipeline first so the aggregator stops receiving new frames,
    // then stop the publisher (which performs a final drain + flush).
    //
    // 关键: 必须阻塞等待 NULL 状态切换真正完成后再 unref。set_state 对
    // 某些异步元素会返回 GST_STATE_CHANGE_ASYNC, 若不 get_state 等待,
    // 进程可能在 v4l2h264dec 尚未关闭 VPU 会话时就退出, 导致下次启动
    // open('/dev/video32') 返回 ENOMEM(会话泄漏)。
    GstStateChangeReturn scr =
        gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
    if (scr == GST_STATE_CHANGE_FAILURE)
      std::cerr << "[WARN] set_state(NULL) returned FAILURE" << std::endl;
    {
      GstState st_cur = GST_STATE_VOID_PENDING;
      GstState st_pend = GST_STATE_VOID_PENDING;
      // 上限 10s: 避免个别元素卡死导致无限阻塞; 正常应瞬时完成。
      GstStateChangeReturn gsr = gst_element_get_state(
          build_result.value.pipeline, &st_cur, &st_pend, 10 * GST_SECOND);
      if (gsr != GST_STATE_CHANGE_SUCCESS || st_cur != GST_STATE_NULL)
        std::cerr << "[WARN] pipeline did not reach NULL cleanly (ret=" << gsr
                  << " state=" << gst_element_state_get_name(st_cur)
                  << "); VPU sessions may not be fully released" << std::endl;
      else
        std::cout << "[INFO] Pipeline reached NULL; VPU decode sessions released."
                  << std::endl;
    }
#ifdef HAVE_DETECTION_NATS
    snapshot_service.Stop();
    aibox::media::AnomalySnapshotService::SetProcessInstance(nullptr);
#endif
    publisher.Stop();
#ifdef HAVE_DETECTION_NATS
    detection_nats.Disconnect();
#endif

    // Report what the aggregator collected + what was published.
    g_aggregator.LogSummary();
#ifdef HAVE_DETECTION_NATS
    std::cout << "[INFO] FrameEventPublisher: published=" << publisher.Published()
              << " republished=" << publisher.Republished()
              << " build_failures=" << publisher.BuildFailures()
              << " publish_failures=" << publisher.PublishFailures() << std::endl;
    std::cout << "[INFO] CaptureTimestampTracker records=" << capture_tracker.Records()
              << " misses=" << capture_tracker.Misses()
              << " regress_fixes=" << capture_tracker.RegressFixes() << std::endl;
    std::cout << "[INFO] AnomalySnapshotService enqueued=" << snapshot_service.Enqueued()
              << " written_ok=" << snapshot_service.WrittenOk()
              << " write_failures=" << snapshot_service.WriteFailures()
              << " published=" << snapshot_service.Published()
              << " dropped=" << snapshot_service.Dropped()
              << " publish_failures=" << snapshot_service.PublishFailures()
              << std::endl;
#else
    std::cout << "[INFO] Publisher: published=" << publisher.Published()
              << " build_failures=" << publisher.BuildFailures()
              << " sink_written=" << event_sink.Written() << std::endl;
#endif
#if defined(HAVE_DETECTION_NATS) && !defined(HAVE_WEBRTC)
    if (!aibox::adapter::NatsAdapter::CloseLibraryAndWait()) {
      std::cerr << "[WARN] NATS internal threads did not stop cleanly"
                << std::endl;
    }
#endif
#ifdef HAVE_DETECTION_NATS
    aibox::detection::CaptureTimestampTracker::SetProcessInstance(nullptr);
#endif

    ReleaseSplitterRequestPads(build_result.value.registry);
    ReleaseBatchRuntime(build_result.value.registry);
    gst_object_unref(build_result.value.pipeline);
    PrintFrameCountSummary();
    aibox::pipeline::PrintStageLatSummary(build_result.value.registry);
    aibox::pipeline::CloseFpsCsv();
    std::cout << "[INFO] Done." << std::endl;
    return exit_code;
  }

  std::cout << "[INFO] AIBox Stream Pipeline Demo — all modules loaded." << std::endl;
#ifdef HAVE_WEBRTC
  ShutdownWebrtcAndNats();
#endif
  gst_element_set_state(build_result.value.pipeline, GST_STATE_NULL);
  gst_element_get_state(build_result.value.pipeline, nullptr, nullptr,
                        10 * GST_SECOND);
  PrintBatchRuntimeStats(build_result.value.registry);
  aibox::pipeline::PrepareBatchTeardown(build_result.value.registry);
#ifdef HAVE_DETECTION_NATS
  aibox::detection::CaptureTimestampTracker::SetProcessInstance(nullptr);
#endif
  ReleaseSplitterRequestPads(build_result.value.registry);
  ReleaseBatchRuntime(build_result.value.registry);
  gst_object_unref(build_result.value.pipeline);
  return 0;
}
