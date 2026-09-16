#!/bin/bash
# collect_error_counts.sh <run_id> <log_file> [more run_id log_file pairs...]
# Appends per-run CSV rows: run_id,e1_mmap,e1_mapgbm,e2_mesa,e3_initial_nd,too_old,gst_errors,segfault,agg
CSV="${AIBOX_ERROR_CSV:-output/error_counts.csv}"
[ -f "$CSV" ] || echo "run_id,e1_mmap,e1_mapgbm,e2_mesa,e3_initial_nd,too_old,gst_errors,segfault,agg" > "$CSV"
while [ "$#" -ge 2 ]; do
  id="$1"; f="$2"; shift 2
  c(){ grep -acE "$1" "$f" 2>/dev/null || echo 0; }
  e1m=$(c 'mmap: Invalid argument')
  e1g=$(c 'MapGbmBufInfoAddress')
  e2m=$(c 'MESA-LOADER')
  e3n=$(c 'initial frames were not dequeued')
  too=$(c 'Too old frames')
  err=$(c ' ERROR |ERROR:|streaming stopped|segment before caps')
  seg=$(c 'Segmentation fault|SIGSEGV')
  agg=$(c '\[AGG\]')
  echo "$id,$e1m,$e1g,$e2m,$e3n,$too,$err,$seg,$agg" >> "$CSV"
  echo "ROW_OK $id $f"
done
echo "CSV=$CSV"
