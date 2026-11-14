#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# orient.py <png> [expect]  — finds the scene's red|green|blue top bar and reports how the picture is rotated/mirrored relative to upright.
# rot = clockwise rotation of the content (0 = upright), mirror = horizontally mirrored. expect like "0", "90", "0m" (m = mirrored).
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); W, H = im.size
s = 400 / max(W, H); im = im.resize((max(1, int(W * s)), max(1, int(H * s))), Image.NEAREST); w, h = im.size
px = im.load(); pts = {'r': [], 'g': [], 'b': []}
for y in range(h):
    for x in range(w):
        R, G, B = px[x, y]
        if R > 170 and G < 80 and B < 80: pts['r'].append((x, y))
        elif G > 170 and R < 90 and B < 90: pts['g'].append((x, y))
        elif B > 170 and R < 80 and G < 80: pts['b'].append((x, y))
n = {k: len(v) for k, v in pts.items()}
if min(n.values()) < max(20, w * h // 4000):
    print(f"NOBAR size={W}x{H} counts={n}"); sys.exit(2)
c = {k: (sum(p[0] for p in v) / len(v), sum(p[1] for p in v) / len(v)) for k, v in pts.items()}
rx, ry = c['b'][0] - c['r'][0], c['b'][1] - c['r'][1]                  # bar direction red -> blue ("right" in the scene)
mx, my = (c['r'][0] + c['g'][0] + c['b'][0]) / 3 - w / 2, (c['r'][1] + c['g'][1] + c['b'][1]) / 3 - h / 2   # bar centre vs picture centre ("up")
ax = lambda x, y: (1 if x > 0 else -1, 0) if abs(x) >= abs(y) else (0, 1 if y > 0 else -1)
r = ax(rx, ry); u = ax(mx, my)
if r[0] * u[0] + r[1] * u[1] != 0:   # bar centre not clear of the centre line: use the perpendicular to r that points to the bar
    u = (r[1], -r[0]) if (r[1] * mx - r[0] * my) > 0 else (-r[1], r[0])
rot = {(0, -1): 0, (1, 0): 90, (0, 1): 180, (-1, 0): 270}[u]
cross = r[0] * u[1] - r[1] * u[0]     # upright, unmirrored: r=(1,0), u=(0,-1) -> -1
mir = cross > 0
res = f"{rot}{'m' if mir else ''}"
exp = sys.argv[2] if len(sys.argv) > 2 else None
print(f"{'PASS' if exp is None or exp == res else 'FAIL'} orient={res}{'' if exp is None else ' expect=' + exp} size={W}x{H}")
sys.exit(0 if exp is None or exp == res else 1)
