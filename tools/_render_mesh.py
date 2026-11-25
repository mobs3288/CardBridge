"""Diagnostic: software-rasterize selected PMX materials (painter's algorithm).

Usage: python tools/_render_mesh.py <pmx> <out.png> <material-index> [...]
"""
import sys
from PIL import Image, ImageDraw
from _geom_audit import parse

pmx, out = sys.argv[1], sys.argv[2]
mats_wanted = {int(x) for x in sys.argv[3:]}
vpos, faces, mats = parse(pmx)
W, H = 700, 900
img = Image.new('RGB', (W, H), (235, 237, 240))
dr = ImageDraw.Draw(img)


def project(p):
    x, y, z = p
    # front view: x to screen x, y up (screen y down), z toward camera (-z = front)
    return (W / 2 + x * 26.0, H - 40 - y * 26.0)


tris = []
fstart = 0
for mi, (name, cnt, memo) in enumerate(mats):
    n = cnt // 3
    if mi in mats_wanted:
        for f in faces[fstart:fstart + n]:
            pts = [project(vpos[vi]) for vi in f]
            depth = sum(vpos[vi][2] for vi in f) / 3.0
            col = [(220, 40, 60), (40, 90, 200), (40, 180, 90), (200, 160, 40), (150, 60, 180)][mi % 5]
            tris.append((depth, pts, col))
    fstart += n

tris.sort(key=lambda t: t[0])  # most negative z (front) drawn last
for depth, pts, col in tris:
    dr.polygon(pts, fill=col)
img.save(out)
print(f'{out}: {len(tris)} triangles')
