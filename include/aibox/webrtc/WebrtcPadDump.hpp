#pragma once

// Diagnostic-only pad dump for WebRTC missing-box investigation.
// Enabled only when env AIBOX_WEBRTC_PAD_DUMP=1 (default OFF; no topology change).
// Probes: P1 overlay src, P2 demux_webrtc src, P2m recording demux src,
//         P3 caps_webrtc src, P4 enc_webrtc sink, P5 enc_webrtc src.

#include <gst/gst.h>
#include <string>

#include "aibox/runtime/RuntimeRegistry.hpp"

namespace aibox::webrtc {

// Attach probes; dumps land under out_dir (created if missing).
// Env:
//   AIBOX_WEBRTC_PAD_DUMP=1          enable
//   AIBOX_WEBRTC_PAD_DUMP_DIR=<path> optional output dir
//   AIBOX_WEBRTC_PAD_DUMP_SKIP=N     frames to skip before dump (default 40)
//   AIBOX_WEBRTC_PAD_DUMP_COUNT=N    frames to dump per pad (default 3)
//   AIBOX_WEBRTC_PAD_DUMP_ONLY=encoded  only P5/P5b (no raw map; DMA-safe test)
// Safe no-op when AIBOX_WEBRTC_PAD_DUMP is unset/0.
void MaybeInstallWebrtcPadDumps(GstElement* pipeline,
                                const aibox::pipeline::RuntimeRegistry& reg,
                                const std::string& out_dir);

// Remove remaining probes (call before pipeline teardown). Idempotent.
void UninstallWebrtcPadDumps();

}  // namespace aibox::webrtc
