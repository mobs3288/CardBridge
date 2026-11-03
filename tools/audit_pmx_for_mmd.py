"""Audits a PMX file for the things MikuMikuDance (7.39) is known to choke on, which the engine's own loader does not mind.

    python -I -X utf8 audit_pmx_for_mmd.py <model.pmx>

Prints a line per finding (ERROR = MMD is likely to crash or refuse the file, WARN = it loads but misbehaves) and the model's sizes.
It reads PMX 2.0 / 2.1 with UTF-16LE text. No third-party packages.
"""
import math
import os
import struct
import sys


class R:
    def __init__(self, d):
        self.d, self.o = d, 0

    def take(self, n):
        b = self.d[self.o:self.o + n]
        if len(b) != n:
            raise EOFError('truncated at %d' % self.o)
        self.o += n
        return b

    def u8(self): return self.take(1)[0]
    def i8(self): return struct.unpack('<b', self.take(1))[0]
    def i16(self): return struct.unpack('<h', self.take(2))[0]
    def u16(self): return struct.unpack('<H', self.take(2))[0]
    def i32(self): return struct.unpack('<i', self.take(4))[0]
    def f32(self): return struct.unpack('<f', self.take(4))[0]
    def v(self, n): return struct.unpack('<%df' % n, self.take(4 * n))

    def text(self, enc):
        n = self.i32()
        return self.take(n).decode('utf-16le' if enc == 0 else 'utf-8', errors='replace')

    def idx(self, size, signed=True):
        if size == 1: return self.i8() if signed else self.u8()
        if size == 2: return self.i16() if signed else self.u16()
        return self.i32()


def audit(path):
    out = []
    def err(m): out.append('ERROR ' + m)
    def warn(m): out.append('WARN  ' + m)
    r = R(open(path, 'rb').read())
    if r.take(4) != b'PMX ':
        err('not a PMX file'); return out
    ver = r.f32()
    n = r.u8()
    g = list(r.take(n))
    enc, addl, vs, ts, ms, bs, mos, rs = g[:8]
    name, name_en, comment, comment_en = (r.text(enc) for _ in range(4))
    info = {'version': ver, 'name': name, 'comment': comment[:60]}
    if ver not in (2.0, 2.1): warn('PMX version %s' % ver)
    if not name.strip(): err('empty model name (MMD shows NoInfo)')
    if not comment.strip(): warn('empty comment')

    nv = r.i32()
    for i in range(nv):
        pos = r.v(3); nrm = r.v(3); uv = r.v(2); r.take(16 * addl)
        wt = r.u8()
        if wt == 0: b = [r.idx(bs)]; w = [1.0]
        elif wt == 1: b = [r.idx(bs), r.idx(bs)]; w0 = r.f32(); w = [w0, 1 - w0]
        elif wt == 2:
            b = [r.idx(bs) for _ in range(4)]; w = list(r.v(4))
            if abs(sum(w) - 1.0) > 0.02 and sum(w) > 0: warn('vertex %d: BDEF4 weights sum %.3f' % (i, sum(w)))
            if sum(w) == 0: err('vertex %d: BDEF4 weights all zero' % i)
        elif wt == 3: b = [r.idx(bs), r.idx(bs)]; w = [r.f32()]; r.take(36)
        elif wt == 4: b = [r.idx(bs) for _ in range(4)]; w = list(r.v(4)); r.take(36)
        else: err('vertex %d: weight type %d' % (i, wt)); return out
        r.f32()
        if any(math.isnan(x) or math.isinf(x) for x in pos + nrm + uv): err('vertex %d: NaN or infinity' % i)
        if i < 1: info['v0'] = pos
    ni = r.i32()
    if ni % 3: err('index count %d is not a multiple of 3' % ni)
    inds = [r.idx(vs, False) if vs != 4 else r.i32() for _ in range(ni)]
    if inds and max(inds) >= nv: err('an index points past the vertices (%d >= %d)' % (max(inds), nv))
    nt = r.i32()
    textures = [r.text(enc) for _ in range(nt)]
    for t in textures:
        if t.startswith(('/', '\\')) or ':' in t: err('texture path is absolute: %s' % t)
        if len(t.encode('utf-8')) > 200: warn('long texture path: %s' % t[:60])
    nm = r.i32()
    total = 0
    for i in range(nm):
        mn = r.text(enc); r.text(enc)
        r.take(16 + 12 + 4 + 12); flag = r.u8(); r.take(16 + 4)
        tex = r.idx(ts); sph = r.idx(ts); r.u8()
        shared = r.u8()
        toon = r.idx(ts) if shared == 0 else r.u8()
        r.text(enc)
        cnt = r.i32(); total += cnt
        if cnt % 3: err('material %d "%s": index count %d not a multiple of 3' % (i, mn, cnt))
        if tex >= nt: err('material %d: texture index %d past %d' % (i, tex, nt))
        if shared == 0 and toon >= nt: err('material %d: toon index past textures' % i)
    if total != ni: err('materials own %d indices but the model has %d' % (total, ni))
    nb = r.i32()
    bones = []
    ik_count = 0
    for i in range(nb):
        bn = r.text(enc); r.text(enc)
        pos = r.v(3); parent = r.idx(bs); layer = r.i32(); flags = r.u16()
        if flags & 1: tail = r.idx(bs)
        else: tail = r.v(3)
        if flags & 0x300:
            r.idx(bs); r.f32()
        if flags & 0x400: r.v(3)
        if flags & 0x800: r.v(3); r.v(3)
        if flags & 0x2000: r.i32()
        ikinfo = None
        if flags & 0x20:
            ik_count += 1
            target = r.idx(bs); loops = r.i32(); lim = r.f32(); nl = r.i32()
            links = []
            for _ in range(nl):
                lb = r.idx(bs); has = r.u8()
                lo = up = None
                if has: lo = r.v(3); up = r.v(3)
                links.append((lb, lo, up))
            ikinfo = (target, loops, lim, links)
            for lb, lo, up in links:
                if lo is not None and any(a > b + 1e-6 for a, b in zip(lo, up)):
                    err('bone %d "%s": IK link limit lower > upper %s %s (MMD crashes on this)' % (i, bn, lo, up))
                if lb < 0 or lb >= nb: err('bone %d: IK link bone %d out of range' % (i, lb))
            if target < 0 or target >= nb: err('bone %d: IK target %d out of range' % (i, target))
            if nl > 20: warn('bone %d: %d IK links' % (i, nl))
            if nl == 0: err('bone %d "%s": IK with no links' % (i, bn))
            if loops > 255: warn('bone %d: IK loop %d' % (i, loops))
        if parent >= nb: err('bone %d: parent %d out of range' % (i, parent))
        if any(math.isnan(x) for x in pos): err('bone %d: NaN position' % i)
        bones.append((bn, parent, flags))
    names = {}
    for i, (bn, parent, flags) in enumerate(bones):
        names.setdefault(bn, []).append(i)
    dup = {k: v for k, v in names.items() if len(v) > 1}
    if dup: warn('%d duplicated bone names, e.g. %s' % (len(dup), list(dup)[:3]))
    seen_cycle = False
    for i in range(nb):
        p, steps = bones[i][1], 0
        while p >= 0 and steps <= nb:
            p = bones[p][1]; steps += 1
        if steps > nb: err('bone parent cycle through %d' % i); seen_cycle = True; break
    long_names = [b[0] for b in bones if len(b[0].encode('shift_jis', errors='replace')) > 15]
    if long_names: warn('%d bone names longer than 15 bytes in Shift-JIS (MMD cuts them): %s' % (len(long_names), long_names[:3]))
    nmo = r.i32()
    morph_names = []
    group_refs = {}
    for i in range(nmo):
        mn = r.text(enc); r.text(enc); panel = r.u8(); typ = r.u8(); cnt = r.i32()
        morph_names.append(mn)
        if cnt == 0: warn('morph %d "%s" is empty' % (i, mn))
        for _ in range(cnt):
            if typ == 0:
                gi = r.idx(mos); r.f32(); group_refs.setdefault(i, []).append(gi)
            elif typ == 1: r.idx(vs, False) if vs != 4 else r.i32(); r.v(3)
            elif typ == 2: r.idx(bs); r.v(3); r.v(4)
            elif typ in (3, 4, 5, 6, 7): r.idx(vs, False) if vs != 4 else r.i32(); r.v(4)
            elif typ == 8:
                m = r.idx(ms); r.u8(); r.take(16 + 12 + 4 + 12 + 16 + 4 + 16 + 16 + 16)
            else: err('morph %d: type %d' % (i, typ)); return out
    for i, refs in group_refs.items():
        for gi in refs:
            if gi >= nmo or gi < 0: err('group morph %d points at %d' % (i, gi))
            elif gi in group_refs: err('group morph %d "%s" contains group morph %d (MMD crashes on nested group morphs)' % (i, morph_names[i], gi))
    ndup = len(morph_names) - len(set(morph_names))
    if ndup: warn('%d duplicated morph names' % ndup)
    nf = r.i32()
    # MMD (9.32) counts the display frames and the morphs they list in one BYTE each, then fills tables sized by that byte: 256 or more
    # overflow the heap (exit -1073740940 a few seconds after the file is opened).
    if nf > 254: err('%d display frames (MMD stores the count in a byte: at most 254)' % nf)
    listed = 0
    for i in range(nf):
        fn = r.text(enc); r.text(enc); special = r.u8(); cnt = r.i32()
        for _ in range(cnt):
            t = r.u8()
            listed += t == 1
            ref = r.idx(bs) if t == 0 else r.idx(mos)
            if t == 0 and not (0 <= ref < nb): err('display frame %d points at bone %d' % (i, ref))
            if t == 1 and not (0 <= ref < nmo): err('display frame %d points at morph %d' % (i, ref))
        if i == 0 and special and cnt == 0: err('the Root display frame is empty')
    if listed > 255: err('display frames list %d morphs (MMD stores the count in a byte: at most 255, more corrupt its heap)' % listed)
    nr = r.i32()
    for i in range(nr):
        rn = r.text(enc); r.text(enc)
        b = r.idx(bs); grp = r.u8(); mask = r.u16(); shape = r.u8(); size = r.v(3); pos = r.v(3); rot = r.v(3)
        mass, ld, ad, rest, fric = r.v(5); mode = r.u8()
        if grp > 15: err('rigid body %d: group %d' % (i, grp))
        if shape not in (0, 1, 2): err('rigid body %d: shape %d' % (i, shape))
        if any(math.isnan(x) or math.isinf(x) for x in size + pos + rot + (mass, ld, ad, rest, fric)): err('rigid body %d "%s": NaN or infinity' % (i, rn))
        if mode != 0 and mass <= 0: warn('rigid body %d "%s": dynamic with mass %.3f' % (i, rn, mass))
        if any(s < 0 for s in size): err('rigid body %d: negative size' % i)
        if b >= nb: err('rigid body %d: bone %d out of range' % (i, b))
    nj = r.i32()
    for i in range(nj):
        jn = r.text(enc); r.text(enc); t = r.u8()
        a = r.idx(rs); b = r.idx(rs); vals = r.v(24)
        if not (0 <= a < nr and 0 <= b < nr): err('joint %d "%s": rigid body %d / %d of %d' % (i, jn, a, b, nr))
        if any(math.isnan(x) or math.isinf(x) for x in vals): err('joint %d "%s": NaN or infinity' % (i, jn))
        lo, up = vals[6:9], vals[9:12]
        if any(x > y + 1e-6 for x, y in zip(lo, up)): warn('joint %d: move limit lower > upper' % i)
        lo, up = vals[12:15], vals[15:18]
        if any(x > y + 1e-6 for x, y in zip(lo, up)): warn('joint %d: rotation limit lower > upper' % i)
    if r.o != len(r.d): warn('%d bytes after the joints (a PMX 2.1 soft body block?)' % (len(r.d) - r.o))
    info.update(vertices=nv, triangles=ni // 3, textures=nt, materials=nm, bones=nb, ik=ik_count, morphs=nmo, frames=nf, rigid=nr, joints=nj)
    out.insert(0, 'INFO  ' + ', '.join('%s=%s' % kv for kv in info.items()))
    return out


if __name__ == '__main__':
    lines = audit(sys.argv[1])
    for l in lines: print(l)
    sys.exit(1 if any(l.startswith('ERROR') for l in lines) else 0)
