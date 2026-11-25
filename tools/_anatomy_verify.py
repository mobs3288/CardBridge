"""Diagnostic: verify the anatomy meshes that reach a PMX.

Usage: python tools/_anatomy_verify.py <file.pmx> [...]

Prints, per body.anatomy material, triangle counts, the bones its vertices are
weighted to and the worst weight-sum error. No card data or binary dumps.
"""
import sys
from _pmx_verts import parse

sys.stdout.reconfigure(encoding='utf-8')

for path in sys.argv[1:]:
    verts, faces, mats, bones = parse(path)
    print(f'{path}: {len(verts)} vertices, {len(faces)} triangles, {len(bones)} bones')
    fstart = 0
    found = 0
    for name, cnt, memo in mats:
        tris = cnt // 3
        vs = set()
        for f in faces[fstart:fstart + tris]:
            vs.update(f)
        fstart += tris
        if not name.startswith('body.anatomy/'):
            continue
        found += tris
        worst = max((abs(sum(v['w'].values()) - 1.0) for v in (verts[i] for i in vs)), default=0)
        used = sorted({bones[b] for i in vs for b in verts[i]['w']})
        print(f'  {name}: {len(vs)} verts, {tris} tris, weight error {worst:.6f}')
        print(f'    bones: {", ".join(used)}')
    print(f'  anatomy total: {found} triangles, {"OK" if found else "MISSING"}')
