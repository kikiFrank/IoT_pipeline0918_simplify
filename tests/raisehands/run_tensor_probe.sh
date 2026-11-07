#!/bin/bash
# Board-only preprocessing/QNN diagnostic; never deployed into 0924.
set -euo pipefail
cd "$(dirname "$0")/../.."
export AIBOX_ROOT="$PWD"
. scripts/runtime_graphics_env.sh
. /tmp/aibox_wayland.env 2>/dev/null || true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export GST_PLUGIN_PATH="$PWD/runtime_plugins" GST_ML_MODULES_DIR="$PWD/plugins/ml_modules"
export COLOR_MEAN="" COLOR_SIGMA=""
export COLOR_MODEL=data/models/raisehands/mobilenetv3_gesture_w8a16.bin
OUT=output/raisehands_acceptance_20261103/tensors
mkdir -p "$OUT"
g++ -std=c++17 tests/andon/color_tensor_probe.cpp src/pipeline/GstElementHelper.cpp -Iinclude \
  -I/mnt/workspace/ppe_imsdk_67189ab/gst-plugin-base \
  $(pkg-config --cflags --libs gstreamer-1.0) \
  /usr/lib/aarch64-linux-gnu/libgstqtimlbase.so.1 -o "$OUT/tensor_probe"
for pattern in black white red green blue; do
  PPE_TEST_PATTERN="$pattern" "$OUT/tensor_probe" 160 320 "$OUT/gesture_$pattern" \
    > "$OUT/gesture_$pattern.log" 2>&1
done
python3 - "$OUT" <<'PY'
import math, pathlib, struct, sys
out = pathlib.Path(sys.argv[1])
mean, std = (0.485,0.456,0.406), (0.229,0.224,0.225)
scale, offset = 7.260096026584506e-05, -29172
def expected(rgb):
    return tuple(max(0,min(65535,round((rgb[c]/255-mean[c])/std[c]/scale-offset))) for c in range(3))
for name, rgb in {'black':(0,0,0),'white':(255,255,255),'red':(255,0,0),'green':(0,255,0),'blue':(0,0,255)}.items():
    raw = (out/f'gesture_{name}_input.raw').read_bytes()
    assert len(raw)==128*128*3*2
    assert struct.unpack_from('<3H',raw,(64*128+64)*6)==expected(rgb), name
    assert struct.unpack_from('<3H',raw,0)==expected((0,0,0)), name
    logits=struct.unpack('<2f',(out/f'gesture_{name}_output.raw').read_bytes())
    assert all(math.isfinite(x) for x in logits), logits
    log=(out/f'gesture_{name}.log').read_text()
    assert 'name=logits' in log and 'dims=1,2,' in log
    print(f'GESTURE_TENSOR_OK pattern={name} RGB_and_normalized_black_padding=OK logits={logits}')
print('GESTURE_TENSOR_PROBE_OK')
PY
