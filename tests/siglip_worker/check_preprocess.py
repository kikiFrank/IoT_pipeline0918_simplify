"""Board-only five-image comparison against the fixed QNN board_001 corpus."""
import array
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys


SOURCE = Path(__file__).resolve().parents[2]
DATASET = Path(os.environ.get("SIGLIP_REFERENCE_DATASET", "/mnt/workspace/1027siglips"))
OUT = SOURCE / "output/siglip_worker/preprocess_test"
OUT.mkdir(parents=True, exist_ok=True)
PROBE = SOURCE / "build-review-siglip/siglip_preprocess_probe"
MODEL = SOURCE / "data/models/siglip2/image_encoder.bin"


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=40)
    if result.returncode:
        raise RuntimeError(f"command failed {command}: {result.stderr[-1200:]!r}")
    return result.stdout


def expected_rgb(path):
    values = array.array("f")
    with path.open("rb") as stream:
        values.fromfile(stream, 224 * 224 * 3)
    return bytes(max(0, min(255, round(float(value) * 255))) for value in values)


rows = [json.loads(line) for _, line in zip(range(5),
                                             (DATASET / "data/manifest.jsonl").open())]
results = []
for row in rows:
    index = row["index"]
    png = OUT / f"{index}.png"
    output = OUT / f"{index}.json"
    run([str(PROBE), str(DATASET / row["image_path"]), str(png)])
    rgb = run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(png),
               "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
    reference_rgb = expected_rgb(DATASET / row["image_raw"])
    if len(rgb) != len(reference_rgb):
        raise RuntimeError(f"RGB byte length differs at {index}")
    errors = [abs(a - b) for a, b in zip(rgb, reference_rgb)]
    maximum = max(errors)
    mean = sum(errors) / len(errors)
    if maximum > 1 or mean > 0.02:
        raise RuntimeError(f"preprocess differs at {index}: max={maximum} mean={mean}")
    tensor = OUT / f"{index}.u16"
    run(["gst-launch-1.0", "-e", "filesrc", f"location={png}", "!", "pngdec", "!",
         "videoconvert", "!", "video/x-raw,format=RGB", "!", "qtivtransform", "!",
         "video/x-raw,format=RGB", "!", "qtimlvconverter", "engine=gles",
         "image-disposition=stretch", "subpixel-layout=regular", "!",
         "neural-network/tensors,type=UINT16", "!", "filesink", f"location={tensor}"])
    native = array.array("H")
    native.frombytes(tensor.read_bytes())
    if len(native) < len(rgb):
        raise RuntimeError("native tensor too short")
    # The DMA allocation includes trailing alignment bytes; logical NHWC data is packed.
    quant_error = max(abs(q - pixel * 257) for q, pixel in zip(native, rgb))
    if quant_error > 257:
        raise RuntimeError(f"native U16 quantization mismatch at {index}: {quant_error}")
    run(["gst-launch-1.0", "-e", "filesrc", f"location={png}", "!", "pngdec", "!",
         "videoconvert", "!", "video/x-raw,format=RGB", "!", "qtivtransform", "!",
         "video/x-raw,format=RGB", "!", "qtimlvconverter", "engine=gles",
         "image-disposition=stretch", "subpixel-layout=regular", "!", "qtimlqnn",
         f"model={MODEL}", "backend=/usr/lib/libQnnHtp.so",
         "system=/usr/lib/libQnnSystem.so", "backend-device-id=1",
         "tensors=<image_embeds>", "!", "qtimlpostprocess_roi", "module=siglip2-image",
         "!", "text/x-raw,format=utf8", "!", "filesink", f"location={output}"])
    embedding = json.loads(output.read_bytes().rstrip(b"\0"))["embedding"]
    reference_path = DATASET / f"results/board_001/image/Result_{index}/image_embeds.raw"
    reference = struct.unpack("<768f", reference_path.read_bytes())
    norm = math.sqrt(sum(value * value for value in reference))
    cosine = sum(a * b / norm for a, b in zip(embedding, reference))
    if cosine < 0.995:
        raise RuntimeError(f"embedding cosine below 0.995 at {index}: {cosine}")
    results.append({"index": index, "max_channel_error": maximum,
                    "mean_channel_error": mean, "native_u16_max_error": quant_error,
                    "cosine": cosine})
print(json.dumps({"status": "PASS", "results": results}, indent=2))
