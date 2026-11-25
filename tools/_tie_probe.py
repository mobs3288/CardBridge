"""Diagnostic: dump the two necktie components of material 34."""
import sys
from _pmx_verts import parse
from _geom_components import components

path = sys.argv[1]
verts, faces, mats, bones = parse(path)
fstart = 0
for mi, (name, cnt, memo) in enumerate(mats):
    tris = cnt // 3
    if mi == 34:
        mfaces = faces[fstart:fstart + tris]
        for ci, comp in enumerate(components(verts and [v['p'] for v in verts], mfaces)[:4]):
            print(f'=== component {ci} nverts={len(comp)} ===')
            ys = [verts[v]['p'][1] for v in comp]
            print('  y range', min(ys), max(ys))
            # histogram of y
            hist = {}
            for v in comp:
                y = round(verts[v]['p'][1] * 2) / 2
                hist[y] = hist.get(y, 0) + 1
            print('  y histogram', sorted(hist.items()))
            for v in comp[:12]:
                vv = verts[v]
                print(f'   v{v} p={tuple(round(x,3) for x in vv["p"])} uv={tuple(round(x,3) for x in vv["uv"])} w={[(bones[b] if b<len(bones) else b, round(w,3)) for b,w in vv["w"].items()]}')
        break
    fstart += tris
