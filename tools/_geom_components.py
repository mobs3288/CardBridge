"""Diagnostic: connected-component geometry stats for one PMX material.

Usage: python tools/_geom_components.py <file.pmx> <material-index>
"""
import sys
from _geom_audit import parse


def components(vpos, faces):
    parent = list(range(len(vpos)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for a, b, c in faces:
        union(a, b); union(b, c)
    used = sorted({v for f in faces for v in f})
    groups = {}
    for i in used:
        groups.setdefault(find(i), []).append(i)
    return sorted(groups.values(), key=len, reverse=True)


def main():
    path = sys.argv[1]
    mi = int(sys.argv[2])
    vpos, faces, mats = parse(path)
    fstart = 0
    for i, (name, cnt, memo) in enumerate(mats):
        tris = cnt // 3
        if i == mi:
            mfaces = faces[fstart:fstart + tris]
            print(f'MATERIAL [{i}] {name}  tris={tris} memo={memo}')
            comps = components(vpos, mfaces)
            print(f'  {len(comps)} connected components')
            for ci, comp in enumerate(comps[:40]):
                pts = [vpos[v] for v in comp]
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; zs = [p[2] for p in pts]
                print(f'  C{ci:2d} n={len(comp):5d} '
                      f'x=[{min(xs):6.2f},{max(xs):6.2f}] '
                      f'y=[{min(ys):6.2f},{max(ys):6.2f}] '
                      f'z=[{min(zs):6.2f},{max(zs):6.2f}] '
                      f'c=({sum(xs)/len(xs):5.2f},{sum(ys)/len(ys):5.2f},{sum(zs)/len(zs):5.2f})')
            return
        fstart += tris


if __name__ == '__main__':
    main()
