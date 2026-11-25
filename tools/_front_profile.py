"""Diagnostic: front-surface z profile of the sailor blouse and tie."""
import sys
from _geom_audit import parse
from _geom_components import components

path = sys.argv[1]
vpos, faces, mats = parse(path)
fstart = 0
for mi, (name, cnt, memo) in enumerate(mats):
    tris = cnt // 3
    if mi in (32, 34):
        mfaces = faces[fstart:fstart + tris]
        print(f'=== material {mi} {name} ===')
        for ci, comp in enumerate(components(vpos, mfaces)[:4]):
            prof = {}
            for v in comp:
                x, y, z = vpos[v]
                if abs(x) > 0.35:
                    continue
                yb = round(y * 2) / 2
                prof.setdefault(yb, []).append(z)
            rows = []
            for yb in sorted(prof):
                zs = prof[yb]
                rows.append(f'  y={yb:5.1f} n={len(zs):3d} z=[{min(zs):6.2f},{max(zs):6.2f}] mean={sum(zs)/len(zs):6.2f}')
            print(f'  comp {ci} n={len(comp)}:')
            for r in rows: print(r)
    fstart += tris
