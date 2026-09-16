#!/usr/bin/env python3
from pathlib import Path
p = Path("/mnt/workspace/8_20/IoT_pipeline0820/output/rule_lights/p1_demo.log")
text = p.read_text(errors="replace")
keys = (
    "overlay_",
    "qtivoverlay",
    "not writable",
    "Failed to apply",
    "face",
    "FACE",
    "class-filter",
    "PIPELINE_READY",
)
for line in text.splitlines():
    if any(k in line for k in keys):
        print(line)
