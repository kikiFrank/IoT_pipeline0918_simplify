import pathlib
import re
import sys

mode = sys.argv[1]
root = pathlib.Path('output/andon_4cam')
log = (root / f'demo_{mode}.log').read_text(errors='replace')
if mode == 'zero':
    assert '[COLOR_INPUT_ENCODING]' not in log and '[ANDON_COLOR]' not in log
frames = re.findall(r'\[ANDON_FRAME\] camera=(cam-\d+) frame=(\d+) rois=(\d+) classified=(\d+)', log)
assert frames, 'No terminal frame evidence'
assert 'ANDON_BINDING_ERROR' not in log
for n in range(1, 5):
    camera = f'cam-{n:03}'
    rows = [(int(rois), int(classes)) for cam, _, rois, classes in frames if cam == camera]
    assert rows, camera
    assert all(rois == classes for rois, classes in rows), (camera, rows)
    if mode == 'zero':
        assert all(rois == 0 for rois, _ in rows), (camera, rows)
        assert (root / 'captures' / mode / f'{camera}.jpg').stat().st_size > 1000
    elif mode == 'single':
        assert all(rois <= 1 for rois, _ in rows) and any(rois == 1 for rois, _ in rows), (camera, rows)
    else:
        assert any(rois > 1 for rois, _ in rows), (camera, rows)
        capture = root / 'captures' / mode / f'{camera}.jpg'
        if not capture.exists():  # Evidence from the initial multi-ROI round.
            capture = root / 'captures' / f'{camera}.jpg'
        assert capture.stat().st_size > 1000
    print(f'FRAME_BINDING_OK mode={mode} camera={camera} samples={len(rows)}')
memory = [list(map(int, line.split())) for line in
          (root / f'memory_{mode}.txt').read_text().splitlines()[1:]]
warm = [row[1] for row in memory[len(memory)//2:]]
growth = warm[-1] - warm[0]
assert growth < 65536, f'RSS grew {growth}KiB after warmup'
print(f'MEMORY_OBSERVATION mode={mode} rss_min_kib={min(warm)} rss_max_kib={max(warm)} late_growth_kib={growth}')
