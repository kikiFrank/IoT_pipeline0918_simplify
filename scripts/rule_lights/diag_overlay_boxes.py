#!/usr/bin/env python3
from pathlib import Path

demo = Path("/mnt/workspace/8_20/IoT_pipeline0820/output/rule_lights/p1_demo.log")
text = demo.read_text(errors="replace")
keys = (
    "[TERMINAL]",
    "[DBG class-filter]",
    "overlay_",
    "not writable",
    "Failed to apply",
    "[face]",
    "PIPELINE_READY",
    "[AGG]",
    "requested=",
    "actual=",
    "libgstqtivoverlay",
    "aiboxqtivoverlay",
    "masks",
    "ROI",
)
print("=== overlay/terminal/face ===")
n = 0
for line in text.splitlines():
    if any(k in line for k in keys):
        print(line)
        n += 1
        if n >= 120:
            print("...truncated...")
            break

print("\n=== counts ===")
for k in ("[TERMINAL]", "roi_total=", "rois=", "not writable", "Failed to apply",
          "[AGG]", "class-filter", "overlay_"):
    print(f"{k}: {text.count(k)}")

print("\n=== last 15 TERMINAL ===")
terms = [ln for ln in text.splitlines() if "[TERMINAL]" in ln]
for ln in terms[-15:]:
    print(ln)

print("\n=== last 8 AGG ===")
aggs = [ln for ln in text.splitlines() if "[AGG]" in ln]
for ln in aggs[-8:]:
    print(ln)
