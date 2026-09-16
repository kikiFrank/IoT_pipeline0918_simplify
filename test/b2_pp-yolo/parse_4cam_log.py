import os
import re
import sys

p = sys.argv[1] if len(sys.argv) > 1 else "/mnt/workspace/8-17/IoT_pipeline0817/output/b2_4cam_qfix.log"
print("exists", os.path.exists(p), "size", os.path.getsize(p) if os.path.exists(p) else 0)
t = open(p, encoding="utf-8", errors="replace").read() if os.path.exists(p) else ""
print("NULL", "Pipeline reached NULL" in t)
print("SEGV", "Segmentation fault" in t)
print("mmap", t.count("mmap: Invalid argument"))
print("SPLIT_LINES")
for line in t.splitlines():
    if "BATCH_SPLIT" in line or "BATCH_STATS" in line or line.startswith("[AGG] SUMMARY"):
        print(line)
print("HELMET", bool(re.search(r"A_helmet_not_worn|B_helmet_worn", t)))
print("PERSON", "D_person" in t)
print("CAMS", sorted(set(re.findall(r"camera=(cam-\d+)", t))))
print("TAIL")
print("\n".join(t.splitlines()[-12:]))
