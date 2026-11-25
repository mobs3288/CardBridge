"""Diagnostic: software-rasterize a CSV of 3D points as a point cloud (front view)."""
import sys
from PIL import Image, ImageDraw

path, out = sys.argv[1], sys.argv[2]
pts = []
for line in open(path):
    x, y, z = map(float, line.strip().split(','))
    pts.append((x, y, z))
W, H = 500, 900
img = Image.new('RGB', (W, H), (240, 240, 240))
dr = ImageDraw.Draw(img)
lo = min(p[1] for p in pts); hi = max(p[1] for p in pts)
s = 600.0 / max(hi - lo, 1e-6)
for x, y, z in pts:
    sx = W / 2 + x * s
    sy = H - 40 - (y - lo) * s
    # colour by depth (z): more negative = more front = darker
    v = int(max(0, min(255, 200 + z * 300)))
    dr.ellipse((sx - 1.5, sy - 1.5, sx + 1.5, sy + 1.5), fill=(v, 40, 60))
img.save(out)
print(out, len(pts), 'y range', lo, hi)
