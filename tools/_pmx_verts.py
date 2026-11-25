"""Diagnostic: dump PMX bones and per-vertex weights, with bounds per bone-set.

Usage: python tools/_pmx_verts.py <file.pmx> [--bone SUBSTR] [--material IDX]
"""
import struct, sys, collections
from _geom_audit import read_str, idx


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'PMX '
    gcount = b[8]
    enc, add, vis, tis, mis, bis, morphis, rbis = struct.unpack_from('<8B', b, 9)
    pos = 9 + gcount
    for _ in range(4): _, pos = read_str(b, pos, enc)
    print(f'  GLOBALS enc={enc} add={add} vis={vis} tis={tis} mis={mis} bis={bis} morphis={morphis} rbis={rbis}', file=sys.stderr)
    nv, = struct.unpack_from('<i', b, pos); pos += 4
    verts = []
    wtypes = collections.Counter()
    for i in range(nv):
        x, y, z = struct.unpack_from('<3f', b, pos); pos += 12
        nx, ny, nz = struct.unpack_from('<3f', b, pos); pos += 12
        u, v = struct.unpack_from('<2f', b, pos); pos += 8
        pos += 16 * add
        wt = b[pos]; pos += 1
        wtypes[wt] += 1
        ws = {}
        if wt == 0:
            bi, pos = idx(b, pos, bis); ws[bi] = 1.0
        elif wt == 1:
            bi, pos = idx(b, pos, bis); bi2, pos = idx(b, pos, bis); w, = struct.unpack_from('<f', b, pos); pos += 4
            ws[bi] = w; ws[bi2] = 1.0 - w
        elif wt in (2, 4):
            four = []
            for _ in range(4):
                bi, pos = idx(b, pos, bis)
                four.append(bi)
            for bi in four:
                w, = struct.unpack_from('<f', b, pos); pos += 4
                if w > 0: ws[bi] = w
        elif wt == 3:
            bi, pos = idx(b, pos, bis); bi2, pos = idx(b, pos, bis); w, = struct.unpack_from('<f', b, pos); pos += 4
            ws[bi] = w; ws[bi2] = 1.0 - w
            pos += 36
        pos += 4
        verts.append({'p': (x, y, z), 'n': (nx, ny, nz), 'uv': (u, v), 'w': ws})
    ni, = struct.unpack_from('<i', b, pos); pos += 4
    faces = []
    for i in range(0, ni, 3):
        a, pos = idx(b, pos, vis); c, pos = idx(b, pos, vis); d, pos = idx(b, pos, vis)
        faces.append((a, c, d))
    nu, = struct.unpack_from('<i', b, pos); pos += 4
    for i in range(nu): _, pos = read_str(b, pos, enc)
    nm, = struct.unpack_from('<i', b, pos); pos += 4
    mats = []
    for i in range(nm):
        n, pos = read_str(b, pos, enc); ne, pos = read_str(b, pos, enc)
        pos += 16 + 12 + 4 + 12 + 1 + 16 + 4
        ti, pos = idx(b, pos, tis); si, pos = idx(b, pos, tis)
        pos += 1 + 1
        if si == 0: _, pos = idx(b, pos, tis)
        else: pos += 1
        memo, pos = read_str(b, pos, enc)
        cnt, = struct.unpack_from('<i', b, pos); pos += 4
        mats.append((n, cnt, memo))
    print(f'  deform types: {dict(wtypes)}', file=sys.stderr)
    print(f'  DEBUG after materials pos={pos}', file=sys.stderr)
    nb, = struct.unpack_from('<i', b, pos); pos += 4
    print(f'  DEBUG nb={nb}', file=sys.stderr)
    bones = []
    for i in range(nb):
        n, pos = read_str(b, pos, enc); ne, pos = read_str(b, pos, enc)
        pos += 12          # position vec3
        pos += bis         # parent index
        pos += 4           # layer
        fl, = struct.unpack_from('<H', b, pos); pos += 2
        if i < 5: print(f'  DBG bone {i} name={n!r} flags=0x{fl:04x} pos={pos}', file=sys.stderr)
        if fl & 0x0001: pos += bis     # tail bone
        else: pos += 12                 # tail offset vec3
        if fl & 0x0300: pos += bis + 4  # append parent + ratio
        if fl & 0x0400: pos += 12       # fixed axis
        if fl & 0x0800: pos += 24       # local axis X + Z
        if fl & 0x2000: pos += 4        # external parent key
        if fl & 0x0020:                 # IK
            pos += bis + 4 + 4
            nl, = struct.unpack_from('<i', b, pos); pos += 4
            if i < 5: print(f'  DBG bone {i} IK nl={nl}', file=sys.stderr)
            for _ in range(nl):
                pos += bis + 1
                lim = b[pos - 1]
                if lim: pos += 24
        bones.append(n)
    return verts, faces, mats, bones


def main():
    path = sys.argv[1]
    verts, faces, mats, bones = parse(path)
    print(f'{len(verts)} verts, {len(faces)} faces, {len(bones)} bones')
    for i, bn in enumerate(bones):
        if 'sailor' in bn.lower() or 'necktie' in bn.lower() or 'tie' in bn.lower():
            print(f'  BONE[{i}] {bn}')
    fstart = 0
    for mi, (name, cnt, memo) in enumerate(mats):
        tris = cnt // 3
        vset = sorted({v for f in faces[fstart:fstart + tris] for v in f})
        fstart += tris
        if mi not in (31, 32, 33, 34):
            continue
        print(f'\nMATERIAL [{mi}] {name} referenced verts={len(vset)}')
        groups = collections.Counter()
        for vi in vset:
            key = tuple(sorted(verts[vi]['w'].items(), key=lambda x: -x[1])[:2])
            groups[key] += 1
        for key, n in groups.most_common(12):
            labels = ', '.join(f'{bones[b] if b < len(bones) else b}:{w:.2f}' for b, w in key)
            print(f'  n={n:5d}  {labels}')


if __name__ == '__main__':
    main()
