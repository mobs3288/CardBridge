"""Render an exported PMX front-view with flat per-material colors to locate artifacts."""
import struct, sys
import numpy as np
from PIL import Image
from _blackband_probe import parse

p = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\pepsi\OneDrive\Desktop\Yoko Littner (3)\Yoko_Littner.pmx'
d = parse(p)
pos, idx, mats = d['pos'], d['idx'], d['mats']
mode = sys.argv[2] if len(sys.argv) > 2 else 'color'

W, H = 600, 1200
# world bounds
xmin, xmax = pos[:, 0].min(), pos[:, 0].max()
ymin, ymax = pos[:, 1].min(), pos[:, 1].max()
sx = (W - 20) / (xmax - xmin)
sy = (H - 20) / (ymax - ymin)
s = min(sx, sy)
ox = W / 2 - (xmin + xmax) / 2 * s
oy = H - 10 - ymax * s

screen = np.empty((len(pos), 2))
screen[:, 0] = pos[:, 0] * s + ox
screen[:, 1] = H - (pos[:, 1] * s + oy)  # flip Y for image

# distinct colors per material
rng = np.random.default_rng(42)
colors = np.array([[0.45, 0.45, 0.45]] * len(mats), np.float32)
for i in range(len(mats)):
    colors[i] = rng.uniform(0.15, 1.0, 3)

img = np.full((H, W, 3), 1.0, np.float32)
zbuf = np.full((H, W), 1e9, np.float32)

def bary(p0, p1, p2, x, y):
    d = (p1[1] - p2[1]) * (p0[0] - p2[0]) + (p2[0] - p1[0]) * (p0[1] - p2[1])
    if abs(d) < 1e-9:
        return None
    l0 = ((p1[1] - p2[1]) * (x - p2[0]) + (p2[0] - p1[0]) * (y - p2[1])) / d
    l1 = ((p2[1] - p0[1]) * (x - p2[0]) + (p0[0] - p2[0]) * (y - p2[1])) / d
    l2 = 1 - l0 - l1
    if l0 < 0 or l1 < 0 or l2 < 0:
        return None
    return l0, l1, l2

for mi, m in enumerate(mats):
    col = colors[mi]
    zc = pos[:, 2].mean()
    for fi in range(m['start'], m['end'], 3):
        a, b, c = idx[fi], idx[fi + 1], idx[fi + 2]
        p0, p1, p2 = screen[a], screen[b], screen[c]
        x0, x1 = max(0, int(np.floor(min(p0[0], p1[0], p2[0])))), min(W - 1, int(np.ceil(max(p0[0], p1[0], p2[0]))))
        y0, y1 = max(0, int(np.floor(min(p0[1], p1[1], p2[1])))), min(H - 1, int(np.ceil(max(p0[1], p1[1], p2[1]))))
        if x1 < x0 or y1 < y0:
            continue
        zz = (pos[a, 2], pos[b, 2], pos[c, 2])
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                w = bary(p0, p1, p2, x + 0.5, y + 0.5)
                if w is None:
                    continue
                z = w[0] * zz[0] + w[1] * zz[1] + w[2] * zz[2]
                if z < zbuf[y, x]:  # smaller z = closer (viewer at -z)
                    zbuf[y, x] = z
                    img[y, x] = col

out = r'C:\Users\pepsi\OneDrive\Desktop\blackband_' + mode + '.png'
Image.fromarray((img * 255).astype(np.uint8)).save(out)
print('saved', out)
for i, m in enumerate(mats):
    print(i, colors[i].round(2).tolist(), m['name'])
