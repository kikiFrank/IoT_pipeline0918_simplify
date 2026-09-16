#!/usr/bin/env python3
from pathlib import Path

root = Path("/mnt/workspace/8_20/IoT_pipeline0820/output/rule_lights")
demo = root / "p1_demo.log"
mon = root / "monitor.log"
print("===DEMO===")
if demo.exists():
    keys = ("FrameEventPublisher", "class-filter", "PIPELINE_READY", "[AGG]", "published=")
    for line in demo.read_text(errors="replace").splitlines():
        if any(k in line for k in keys):
            print(line)
else:
    print("NO_DEMO_LOG")
print("===MON_TAIL===")
if mon.exists():
    lines = mon.read_text(errors="replace").splitlines()
    for line in lines[-25:]:
        print(line)
else:
    print("NO_MON_LOG")
