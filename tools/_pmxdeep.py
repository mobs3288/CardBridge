"""Scratch PMX parser that keeps vertex bone weights, the skeleton and material ranges.

Created during the skirt-fit investigation. Not part of the product.
"""
import struct
import numpy as np


def _read_text(b, o, enc):
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
    name, o = _read_text(b, o, enc)
    _, o = _read_text(b, o, enc)
    _, o = _read_text(b, o, enc)
    _, o = _read_text(b, o, enc)

    nv = struct.unpack_from('<i', b, o)[0]
    o += 4
    pos = np.zeros((nv, 3), np.float32)
    nrm = np.zeros((nv, 3), np.float32)
    uv = np.zeros((nv, 2), np.float32)
    wtype = np.zeros(nv, np.uint8)
    bone = np.full((nv, 4), -1, np.int32)
    weight = np.zeros((nv, 4), np.float32)
    for i in range(nv):
        px, py, pz, nx, ny, nz, u, v = struct.unpack_from('<8f', b, o)
        o += 32
        pos[i] = (px, py, pz)
        nrm[i] = (nx, ny, nz)
        uv[i] = (u, v)
        o += 16 * add_uv
        wt = b[o]
        wtype[i] = wt
        o += 1
        if wt == 0:
            bone[i, 0] = struct.unpack_from('<i', b, o)[0] if bsize == 4 else struct.unpack_from('<H', b, o)[0] if bsize == 2 else b[o]
            weight[i, 0] = 1.0
            o += bsize
        elif wt == 1:
            for k in range(2):
                bone[i, k] = struct.unpack_from('<i', b, o)[0] if bsize == 4 else struct.unpack_from('<H', b, o)[0] if bsize == 2 else b[o]
                o += bsize
            weight[i, 0] = struct.unpack_from('<f', b, o)[0]
            weight[i, 1] = 1.0 - weight[i, 0]
            o += 4
        elif wt in (2, 4):
            for k in range(4):
                bone[i, k] = struct.unpack_from('<i', b, o)[0] if bsize == 4 else struct.unpack_from('<H', b, o)[0] if bsize == 2 else b[o]
                o += bsize
            weight[i, :4] = struct.unpack_from('<4f', b, o)
            o += 16
        elif wt == 3:
            for k in range(2):
                bone[i, k] = struct.unpack_from('<i', b, o)[0] if bsize == 4 else struct.unpack_from('<H', b, o)[0] if bsize == 2 else b[o]
                o += bsize
            weight[i, 0] = struct.unpack_from('<f', b, o)[0]
            weight[i, 1] = 1.0 - weight[i, 0]
            o += 4
            o += 36
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
        s, o = _read_text(b, o, enc)
        textures.append(s)

    nm = struct.unpack_from('<i', b, o)[0]
    o += 4
    tf = {1: '<B', 2: '<H', 4: '<i'}[tsize]
    mats = []
    for _ in range(nm):
        mn, o = _read_text(b, o, enc)
        _, o = _read_text(b, o, enc)
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
        _, o = _read_text(b, o, enc)
        nfc = struct.unpack_from('<i', b, o)[0]
        o += 4
        mats.append({'name': mn, 'tex': tex, 'faces': nfc})
    start = 0
    for m in mats:
        m['start'] = start
        m['verts'] = idx[start:start + m['faces']] if m['faces'] else np.zeros(0, np.int64)
        start += m['faces']

    import os
    if os.environ.get('PMXDEEP_DEBUG'):
        print('DBG after mats', o, 'nb bytes', b[o:o + 8].hex())
    nb = struct.unpack_from('<i', b, o)[0]
    o += 4
    if os.environ.get('PMXDEEP_DEBUG'):
        print('DBG nb', nb, 'bone start', o, b[o:o + 16])
    def ri(at):
        if bsize == 4:
            return struct.unpack_from('<i', b, at)[0]
        if bsize == 2:
            return struct.unpack_from('<H', b, at)[0]
        return b[at]

    bones = []
    for _ in range(nb):
        bn, o = _read_text(b, o, enc)
        be, o = _read_text(b, o, enc)
        bp = struct.unpack_from('<3f', b, o)
        o += 12
        parent = ri(o)
        o += bsize
        layer = struct.unpack_from('<i', b, o)[0]
        o += 4
        flags = struct.unpack_from('<H', b, o)[0]
        o += 2
        tail = None
        tailoff = None
        if flags & 0x0001:
            tail = ri(o)
            o += bsize
        else:
            tailoff = struct.unpack_from('<3f', b, o)
            o += 12
        append_parent = None
        append_rate = None
        if flags & 0x0300:
            append_parent = ri(o)
            o += bsize
            append_rate = struct.unpack_from('<f', b, o)[0]
            o += 4
        fixed = None
        if flags & 0x0400:
            fixed = struct.unpack_from('<3f', b, o)
            o += 12
        local = None
        if flags & 0x0800:
            local = struct.unpack_from('<6f', b, o)
            o += 24
        if flags & 0x2000:
            o += bsize
        ik = None
        if flags & 0x0020:
            target = ri(o)
            o += bsize
            loop = struct.unpack_from('<i', b, o)[0]
            limit = struct.unpack_from('<f', b, o + 4)[0]
            o += 8
            links = []
            nl = struct.unpack_from('<i', b, o)[0]
            o += 4
            for _ in range(nl):
                lb = ri(o)
                o += bsize
                hl = b[o]
                o += 1
                lo = hi = None
                if hl:
                    lo, hi = struct.unpack_from('<3f', b, o), struct.unpack_from('<3f', b, o + 12)
                    o += 24
                links.append((lb, hl, lo, hi))
            ik = (target, loop, limit, links)
        bones.append({'name': bn, 'nameEn': be, 'pos': np.array(bp), 'parent': parent, 'layer': layer,
                      'flags': flags, 'tail': tail, 'tailoff': tailoff, 'appendParent': append_parent,
                      'appendRate': append_rate, 'fixed': fixed, 'local': local, 'ik': ik})
    return {'name': name, 'pos': pos, 'nrm': nrm, 'uv': uv, 'idx': idx, 'mats': mats,
            'textures': textures, 'bones': bones, 'wtype': wtype, 'bone': bone, 'weight': weight}


def material_for_mesh(model, sub):
    for i, m in enumerate(model['mats']):
        if sub in m['name']:
            return i
    return -1


def mat_tris(model, mi):
    m = model['mats'][mi]
    return model['pos'][m['verts'].reshape(-1, 3)]


def weights_of_material(model, mi, top=12):
    m = model['mats'][mi]
    acc = {}
    total = 0.0
    for k in range(m['start'], m['start'] + m['faces']):
        vi = int(model['idx'][k])
        for j in range(4):
            bn = int(model['bone'][vi, j])
            w = float(model['weight'][vi, j])
            if bn >= 0 and w > 0:
                acc[bn] = acc.get(bn, 0.0) + w
                total += w
    rows = sorted(acc.items(), key=lambda kv: -kv[1])
    out = []
    for bn, w in rows[:top]:
        nm = model['bones'][bn]['name'] if bn < len(model['bones']) else f'?{bn}'
        out.append((nm, bn, 100.0 * w / total if total else 0.0))
    return out


def vertex_weights(model, vi):
    out = []
    for j in range(4):
        bn = int(model['bone'][vi, j])
        w = float(model['weight'][vi, j])
        if bn >= 0 and w > 0:
            nm = model['bones'][bn]['name'] if bn < len(model['bones']) else f'?{bn}'
            out.append((nm, bn, w))
    return out


if __name__ == '__main__':
    import sys
    d = parse(sys.argv[1] if len(sys.argv) > 1 else 'out/kk_custom/Oosuki_Mamako.pmx')
    print(d['name'], len(d['pos']), 'verts', len(d['idx']) // 3, 'tris', len(d['bones']), 'bones')
    for i, m in enumerate(d['mats']):
        print(i, repr(m['name']), 'faces', m['faces'])
