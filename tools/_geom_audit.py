"""Diagnostic: dump per-material geometry stats from a PMX file.

Usage: python tools/_geom_audit.py <file.pmx> [material-substring-filter]
"""
import struct, sys, math


def read_str(b, pos, enc):
    n = struct.unpack_from('<i', b, pos)[0]; pos += 4
    s = b[pos:pos + n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace'); pos += n
    return s, pos


def idx(b, pos, size):
    v = {1: b[pos], 2: struct.unpack_from('<H', b, pos)[0], 4: struct.unpack_from('<I', b, pos)[0]}[size]
    return v, pos + size


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'PMX ', b[:4]
    gcount = b[8]
    enc, add, vis, tis, mis, bis, morphis, rbis = struct.unpack_from('<8B', b, 9)
    pos = 9 + gcount
    for _ in range(4):
        _, pos = read_str(b, pos, enc)
    nv, = struct.unpack_from('<i', b, pos); pos += 4
    vpos = []
    for i in range(nv):
        x, y, z = struct.unpack_from('<3f', b, pos); pos += 12
        pos += 12  # normal
        pos += 8   # uv
        pos += 16 * add
        wt = b[pos]; pos += 1
        if wt == 0:
            _, pos = idx(b, pos, bis)
        elif wt == 1:
            _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4
        elif wt in (2, 4):
            for _ in range(4): _, pos = idx(b, pos, bis)
            pos += 16
        elif wt == 3:
            _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4
            pos += 36
        pos += 4
        vpos.append((x, y, z))
    ni, = struct.unpack_from('<i', b, pos); pos += 4
    faces = []
    for i in range(0, ni, 3):
        a, pos = idx(b, pos, vis); c, pos = idx(b, pos, vis); d, pos = idx(b, pos, vis)
        faces.append((a, c, d))
    nu, = struct.unpack_from('<i', b, pos); pos += 4
    for i in range(nu):
        _, pos = read_str(b, pos, enc)
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
    return vpos, faces, mats


def stats(pts):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; zs = [p[2] for p in pts]
    return (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs),
            sum(xs) / len(xs), sum(ys) / len(ys), sum(zs) / len(zs))


def main():
    path = sys.argv[1]
    filt = sys.argv[2].lower() if len(sys.argv) > 2 else ''
    vpos, faces, mats = parse(path)
    print(f'FILE {path}: {len(vpos)} vertices, {len(faces)} triangles, {len(mats)} materials')
    fstart = 0
    for mi, (name, cnt, memo) in enumerate(mats):
        tris = cnt // 3
        pts = [vpos[vi] for f in faces[fstart:fstart + tris] for vi in f]
        fstart += tris
        if filt and filt not in name.lower():
            continue
        if not pts:
            print(f'[{mi:3d}] {name[:48]:48s} EMPTY')
            continue
        x0, x1, y0, y1, z0, z1, cx, cy, cz = stats(pts)
        print(f'[{mi:3d}] {name[:48]:48s} tris={tris:6d} '
              f'x=[{x0:7.2f},{x1:7.2f}] y=[{y0:7.2f},{y1:7.2f}] z=[{z0:7.2f},{z1:7.2f}] '
              f'centroid=({cx:6.2f},{cy:6.2f},{cz:6.2f}) memo={memo[:40]}')


if __name__ == '__main__':
    main()
