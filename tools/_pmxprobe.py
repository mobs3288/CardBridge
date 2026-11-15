import struct, sys
import numpy as np
from PIL import Image


def read_text(b, o, enc):
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    s = b[o:o + n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace')
    return s, o + n


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'PMX ', b[:4]
    o = 4
    ver = struct.unpack_from('<f', b, o)[0]
    o += 4
    gc = b[o]
    o += 1
    globals_ = list(b[o:o + gc])
    o += gc
    enc = globals_[0]
    add_uv = globals_[1]
    vsize, tsize, msize, bsize, gsize, rsize = globals_[2:8]
    o = 4 + 4 + 1 + gc  # after header, before model name
    name, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    _, o = read_text(b, o, enc)
    nv = struct.unpack_from('<i', b, o)[0]
    o += 4
    pos = np.zeros((nv, 3), np.float32)
    nrm = np.zeros((nv, 3), np.float32)
    uv = np.zeros((nv, 2), np.float32)
    for i in range(nv):
        px, py, pz, nx, ny, nz, u, v = struct.unpack_from('<8f', b, o)
        o += 32
        pos[i] = (px, py, pz)
        nrm[i] = (nx, ny, nz)
        uv[i] = (u, v)
        o += 16 * add_uv
        wtype = b[o]
        o += 1
        if wtype == 0:
            o += bsize
        elif wtype == 1:
            o += bsize * 2 + 4
        elif wtype == 2:
            o += bsize * 4 + 16
        elif wtype == 3:
            o += bsize * 2 + 4 + 36
        elif wtype == 4:
            o += bsize * 4 + 16
        o += 4  # edge scale
    nf = struct.unpack_from('<i', b, o)[0]
    o += 4
    fmt = {1: '<B', 2: '<H', 4: '<i'}[vsize]
    idx = np.zeros(nf, np.int64)
    for i in range(nf):
        idx[i] = struct.unpack_from(fmt, b, o)[0]
        o += vsize
    nt = struct.unpack_from('<i', b, o)[0]
    o += 4
    textures = []
    for _ in range(nt):
        s, o = read_text(b, o, enc)
        textures.append(s)
    nm = struct.unpack_from('<i', b, o)[0]
    o += 4
    tf = {1: '<B', 2: '<H', 4: '<i'}[tsize]
    mats = []
    for _ in range(nm):
        mn, o = read_text(b, o, enc)
        _, o = read_text(b, o, enc)
        o += 16 + 12 + 4 + 12 + 1 + 16 + 4
        tex = struct.unpack_from(tf, b, o)[0]
        o += tsize
        sph = struct.unpack_from(tf, b, o)[0]
        o += tsize
        sphmode = b[o]
        o += 1
        toonflag = b[o]
        o += 1
        if toonflag == 0:
            toon = b[o]
            o += 1
        else:
            toon = struct.unpack_from(tf, b, o)[0]
            o += tsize
        _, o = read_text(b, o, enc)
        nfc = struct.unpack_from('<i', b, o)[0]
        o += 4
        mats.append({'name': mn, 'tex': tex, 'faces': nfc})
    start = 0
    for m in mats:
        m['start'] = start
        m['verts'] = idx[start:start + m['faces']] if m['faces'] else np.zeros(0, np.int64)
        start += m['faces']
    return {'name': name, 'pos': pos, 'nrm': nrm, 'uv': uv, 'idx': idx, 'mats': mats, 'textures': textures}


if __name__ == '__main__':
    p = sys.argv[1] if len(sys.argv) > 1 else r'D:\Project K\cardbridge\out\skintest_shade\Himekawa_Akeno.pmx'
    d = parse(p)
    print(d['name'], len(d['pos']), 'verts', len(d['idx']) // 3, 'tris')
    for i, m in enumerate(d['mats']):
        print(i, repr(m['name']), 'faces', m['faces'], 'tex', d['textures'][m['tex']] if 0 <= m['tex'] < len(d['textures']) else None)
