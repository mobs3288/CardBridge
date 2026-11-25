"""Probe materials/geometry of an exported PMX to locate the black band."""
import struct, sys
import numpy as np


def read_text(b, o, enc):
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    s = b[o:o + n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace')
    return s, o + n


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'PMX ', b[:4]
    o = 4
    ver = struct.unpack_from('<f', b, o)[0]; o += 4
    gc = b[o]; o += 1
    g = list(b[o:o + gc]); o += gc
    enc, add_uv = g[0], g[1]
    vsize, tsize, msize, bsize, gsize, rsize = g[2:8]
    o = 4 + 4 + 1 + gc
    name, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    nv = struct.unpack_from('<i', b, o)[0]; o += 4
    pos = np.zeros((nv, 3), np.float32)
    nrm = np.zeros((nv, 3), np.float32)
    uv = np.zeros((nv, 2), np.float32)
    for i in range(nv):
        px, py, pz = struct.unpack_from('<3f', b, o); o += 12
        pos[i] = (px, py, pz)
        nx, ny, nz = struct.unpack_from('<3f', b, o); o += 12
        nrm[i] = (nx, ny, nz)
        u, v = struct.unpack_from('<2f', b, o); o += 8
        uv[i] = (u, v)
        o += 16 * add_uv
        w = b[o]; o += 1
        if w == 0: o += bsize
        elif w == 1: o += bsize * 2 + 4
        elif w == 2: o += bsize * 4 + 16
        elif w == 3: o += bsize * 2 + 4 + 36
        elif w == 4: o += bsize * 4 + 16
        o += 4  # edge scale
    nf = struct.unpack_from('<i', b, o)[0]; o += 4
    fmt = {1: '<B', 2: '<H', 4: '<i'}[vsize]
    idx = np.zeros(nf, np.int64)
    for i in range(nf):
        idx[i] = struct.unpack_from(fmt, b, o)[0]; o += vsize
    nt = struct.unpack_from('<i', b, o)[0]; o += 4
    textures = []
    for _ in range(nt):
        s, o = read_text(b, o, enc); textures.append(s)
    nm = struct.unpack_from('<i', b, o)[0]; o += 4
    tf = {1: '<B', 2: '<H', 4: '<i'}[tsize]
    mats = []
    for _ in range(nm):
        mn, o = read_text(b, o, enc)
        _, o = read_text(b, o, enc)
        diff = struct.unpack_from('<4f', b, o); o += 16
        spec = struct.unpack_from('<3f', b, o); o += 12
        specpower = struct.unpack_from('<f', b, o)[0]; o += 4
        amb = struct.unpack_from('<3f', b, o); o += 12
        flags = b[o]; o += 1
        edge = struct.unpack_from('<4f', b, o); o += 16
        edge_size = struct.unpack_from('<f', b, o)[0]; o += 4
        tex = struct.unpack_from(tf, b, o)[0]; o += tsize
        sph = struct.unpack_from(tf, b, o)[0]; o += tsize
        sphmode = b[o]; o += 1
        toonflag = b[o]; o += 1
        if toonflag == 0: o += 1
        else: o += tsize
        memo, o = read_text(b, o, enc)
        nfc = struct.unpack_from('<i', b, o)[0]; o += 4
        mats.append(dict(name=mn, diff=diff, amb=amb, flags=flags, edge=edge,
                         edge_size=edge_size, tex=tex, faces=nfc, memo=memo))
    start = 0
    for m in mats:
        m['start'] = start
        m['end'] = start + m['faces']
        vs = np.unique(idx[start:start + m['faces']])
        if len(vs):
            m['ymin'], m['ymax'] = float(pos[vs, 1].min()), float(pos[vs, 1].max())
            m['yavg'] = float(pos[vs, 1].mean())
            m['xmin'], m['xmax'] = float(pos[vs, 0].min()), float(pos[vs, 0].max())
        else:
            m['ymin'] = m['ymax'] = m['yavg'] = 0
        start += m['faces']
    return dict(name=name, pos=pos, nrm=nrm, uv=uv, idx=idx, mats=mats, textures=textures, ver=ver)


if __name__ == '__main__':
    p = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\pepsi\OneDrive\Desktop\Yoko Littner (3)\Yoko_Littner.pmx'
    d = parse(p)
    print(d['name'], 'v=', len(d['pos']), 'f=', len(d['idx']) // 3, 'mats=', len(d['mats']), 'ver=', d['ver'])
    print(f"{'#':>3} {'name':<38} {'diff':<24} {'flg':>3} {'tex':<34} {'faces':>6} {'y[min,max]':>16}")
    for i, m in enumerate(d['mats']):
        t = d['textures'][m['tex']] if 0 <= m['tex'] < len(d['textures']) else '-'
        diff = ' '.join(f'{v:.2f}' for v in m['diff'])
        print(f"{i:>3} {m['name'][:38]:<38} {diff:<24} {m['flags']:>3} {t[-34:]:<34} {m['faces']:>6} [{m['ymin']:.2f},{m['ymax']:.2f}]")
