## Quick Start

Self-contained package: the binary, plugins, config snapshots and models all
live under this root and are referenced by relative paths only. Rename the
directory or copy it to another board as-is; nothing points outside it.

```bash
# Start (no environment variables required). Run from this directory:
bash scripts/yolo26/webrtc_start12.sh

# Ready when: BOOT_OK → AGG_OK count=12 → READY_FOR_BROWSER

# Stop (SIGTERM only, never SIGKILL)
bash scripts/webrtc_stop.sh
# Success when: LAST_NULL=clean and no leftover process
```

`webrtc_start12.sh` locates the package root from its own location, so it works
from any working directory and from any absolute path.

## Layout

```
aibox_pipeline/
├── build_unified/        # main binary
├── runtime_plugins/      # custom GStreamer plugins
├── plugins/              # postprocess modules + face preprocess kernels
├── config_snapshots/     # 12cam pipeline config (snapshot/models/labels)
├── data/models/          # detection + face models
├── scripts/              # start/stop scripts
├── runtime_registry/     # GStreamer registry cache
└── output/               # runtime logs
```

## Scripts

| Script | Purpose | How to run |
|---|---|---|
| `yolo26/webrtc_start12.sh` | Start 12-cam pipeline | Run directly ✅ |
| `webrtc_stop.sh` | Graceful stop | Run directly ✅ |
| `webrtc_start_common.sh` | Shared start logic | Sourced by start12 ⚠️ Do not run unless needed |
| `runtime_graphics_env.sh` | Graphics env vars | Sourced ⚠️ Do not run unless needed |
| `aibox_graphics_gate.sh` | Graphics runtime gate | Sourced ⚠️ Do not run unless needed |
| `vpu_health.sh` | VPU health probe | Sourced ⚠️ Do not run unless needed |
| `_start_adreno_weston_forceon.sh` | Emergency: force-start weston | ⛔ Maintainer only, when weston is down |

## Exit Codes

| Code | Meaning |
|---|---|
| 3 | Package root not found (no build_unified/ + config_snapshots/ above the script) |
| 4 | Previous process not stopped (STOP_STUCK) |
| 5 | NATS 4222/8443 not ready |
| 7 | Graphics runtime gate failed |
| 8 | Boot / aggregation timeout |
| 9 | AGG aggregation timeout |
| 11 | Missing binary or snapshot file |

## Requirements

- weston (systemd autostart) + seatd
- `gstreamer1.0-nice`, QTI plugins 1.8.1, QAIRT 2.46
- NATS (4222 + WS 8443)
