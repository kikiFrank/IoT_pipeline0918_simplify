import math
import pathlib
import struct

root = pathlib.Path('output/andon_4cam')
mean = (0.485, 0.456, 0.406)
std = (0.229, 0.224, 0.225)
scale, offset = 7.260096026584506e-05, -29172
patterns = {'black': (0, 0, 0), 'white': (255, 255, 255),
            'red': (255, 0, 0), 'green': (0, 255, 0), 'blue': (0, 0, 255)}

def expected(rgb):
    return tuple(max(0, min(65535, round((rgb[c] / 255 - mean[c]) / std[c] / scale - offset)))
                 for c in range(3))

for name, rgb in patterns.items():
    raw = (root / f'color_{name}_input.raw').read_bytes()
    assert len(raw) == 128 * 128 * 3 * 2
    center = struct.unpack_from('<3H', raw, (64 * 128 + 64) * 6)
    border = struct.unpack_from('<3H', raw, 0)
    assert center == expected(rgb), (name, center, expected(rgb))
    assert border == expected((0, 0, 0)), (name, border)
    logits = struct.unpack('<8f', (root / f'color_{name}_output.raw').read_bytes())
    assert all(math.isfinite(v) for v in logits), logits
    print(f'COLOR_ENCODING_OK pattern={name} center={center} padding={border} logits=8finite')

