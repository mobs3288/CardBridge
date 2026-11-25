"""Diagnostic: zoomed independent render of PMX anatomy geometry (front view).

Usage: python tools/_anatomy_view.py <pmx> <out.png> [scale] [center_y]
"""
import sys
from PIL import Image, ImageDraw
from _geom_audit import parse

pmx, out = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 220.0
cy = float(sys.argv[4]) if len(sys.argv) > 4 else 9.9
vpos, faces, mats = parse(pmx)
W, H = 700, 700
img = Image.new('RGB', (W, H), (235, 237, 240))
dr = ImageDraw.Draw(img)


def project(p):
    return (W / 2 + p[0] * scale, H / 2 - (p[1] - cy) * scale)


tris = []
fstart = 0
for mi, (name, cnt, memo) in enumerate(mats):
    n = cnt // 3
    if 'anatomy' in name or name.startswith('body/'):
        for f in faces[fstart:fstart + n]:
            pts = [project(vpos[vi]) for vi in f]
            depth = sum(vpos[vi][2] for vi in f) / 3.0
            col = (40, 180, 90) if 'anatomy' in name else (245, 205, 195)
            tris.append((depth, pts, col))
    fstart += n

tris.sort(key=lambda t: t[0])
for depth, pts, col in tris:
    dr.polygon(pts, fill=col)
img.save(out)
print(f'{out}: {len(tris)} triangles')
