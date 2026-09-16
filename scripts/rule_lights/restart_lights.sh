#!/bin/bash
set -euo pipefail
R=/mnt/workspace/8_20/IoT_pipeline0820
echo "== apply rule_engine conf 0.5 =="
bash "$R/scripts/rule_lights/apply_rule_engine.sh"
echo "== stop pipeline =="
bash "$R/scripts/webrtc_stop.sh" || true
echo "== start lights =="
bash "$R/scripts/rule_lights/rule_lights_start1.sh"
echo "== restart monitor =="
bash "$R/scripts/rule_lights/start_monitor.sh"
