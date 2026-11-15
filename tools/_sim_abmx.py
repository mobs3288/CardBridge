"""Scratch: simulate ABMX bone scaling on an exported mesh (investigation only).

The exported PMX is skinned at rest. ABMX scale-only modifiers change the bone local scale; with
identity rest rotations the effect on a vertex is a linear blend of per-bone matrices
W_final * W_base^-1. Re-applying a modifier to an export that has none reproduces the other
export, which validates the model.
"""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _abmx as abmxlib
import _clothing_abmx as clolib
import _fitmetric as fm
import _pmxdeep as pmx

CARD = r'C:\Users\pepsi\Downloads\KK_236802 (1).png'
CLOTHES = r'C:\Users\pepsi\Downloads\KKCLOTHING_012669.png'
O1 = 'out/kk_custom_o1/Oosuki_Mamako.pmx'
O6 = 'out/kk_custom/Oosuki_Mamako.pmx'

IDENT = (1.0, 1.0, 1.0)


def coord_scale(mods, coord):
    """What KKABMX's BoneModifier.GetModifier returns for this coordinate."""
    if len(mods) == 1:
        m = mods[0]
    elif coord < len(mods):
        m = mods[coord]
    else:
        return IDENT
    return tuple(float(x) for x in (m[0] if len(m) > 0 else [1, 1, 1]))


def load_card_table(path):
    marker, ab = abmxlib.load_abmx(path)
    return ab


def load_clothes_table(path):
    marker, ext = clolib.read_clothing(path)
    ent = ext['KKABMPlugin.ABMData']
    bd = ent[1]['boneData']
    import io
    import msgpack
    if bd[0] in (0xc7, 0xc8, 0xc9):
        lb = {0xc7: 1, 0xc8: 2, 0xc9: 4}[bd[0]]
        assert bd[1 + lb] == 99
        o = 1 + lb + 1
        un = msgpack.Unpacker(io.BytesIO(bd[o:]), raw=False, strict_map_key=False)
        ulen = un.unpack()
        o += un.tell()
        raw = abmxlib.lz4_decompress_block(bd[o:], ulen)
        obj = msgpack.unpackb(raw, raw=False, strict_map_key=False)
    else:
        obj = msgpack.unpackb(bd, raw=False, strict_map_key=False)
    assert isinstance(obj, dict)
    return {name: tuple(float(x) for x in m[0]) for name, m in obj.items()}


def bone_matrices(d, scales):
    n = len(d['bones'])
    abs_pos = np.array([b['pos'] for b in d['bones']], float)
    local = abs_pos.copy()
    for i, b in enumerate(d['bones']):
        if 0 <= b['parent'] < n:
            local[i] = abs_pos[i] - abs_pos[b['parent']]
    wb = [None] * n
    wf = [None] * n
    for i, b in enumerate(d['bones']):
        t = np.eye(4)
        t[:3, 3] = local[i]
        s = np.eye(4)
        s[0, 0], s[1, 1], s[2, 2] = scales.get(b['name'], IDENT)
        if 0 <= b['parent'] < n:
            wb[i] = wb[b['parent']] @ t
            wf[i] = wf[b['parent']] @ t @ s
        else:
            wb[i] = t
            wf[i] = t @ s
    return [wf[i] @ np.linalg.inv(wb[i]) for i in range(n)]


def skin(d, vidx, mats):
    p = np.concatenate([d['pos'][vidx], np.ones((len(vidx), 1))], axis=1)
    out = np.zeros((len(vidx), 4))
    for j in range(4):
        bi = d['bone'][vidx, j]
        w = d['weight'][vidx, j]
        ok = (bi >= 0) & (w > 0)
        for k in np.nonzero(ok)[0]:
            out[k] += w[k] * (mats[int(bi[k])] @ p[k])
    return out[:, :3]


def garment_arrays(d, sub):
    gi = next(i for i, m in enumerate(d['mats']) if sub in m['name'])
    idx = d['mats'][gi]['verts']
    vidx = np.unique(idx)
    local = {int(v): k for k, v in enumerate(vidx)}
    tris = np.array([[local[int(v)] for v in t] for t in idx.reshape(-1, 3)])
    return vidx, tris


def measure_tris(body_tris, gar_tris, center, angles=720, slices=100, cap=0.5, tol=0.005, verbose=True):
    y0, y1 = gar_tris[:, :, 1].min(), gar_tris[:, :, 1].max()
    span = y1 - y0
    ys = np.linspace(y0 + span * 0.03, y1 - span * 0.02, slices)
    th = np.linspace(0, 2 * np.pi, angles, endpoint=False)
    rows = []
    for yp in ys:
        rbs = fm.ray_radii(body_tris, yp, center, th)
        rgs = fm.ray_radii(gar_tris, yp, center, th)
        rb = np.array([rr[-1] if len(rr) else np.nan for rr in rbs])
        rg = np.array([rr[-1] if len(rr) else np.nan for rr in rgs])
        near = np.full(len(th), np.nan)
        for ai in range(len(th)):
            if not np.isfinite(rg[ai]):
                continue
            cand = rbs[ai][(rbs[ai] <= rg[ai] + cap)]
            if len(cand):
                near[ai] = cand[-1]
        viol = np.isfinite(rg) & np.isfinite(near) & (near > rg + tol)
        dlt = np.where(viol, near - rg, np.nan)
        rows.append((yp, int(viol.sum()),
                     float(np.nanmax(dlt)) if viol.any() else 0.0,
                     float(np.nanmean(dlt)) if viol.any() else 0.0))
    total = sum(r[1] for r in rows)
    samples = len(rows) * angles
    worst = max(rows, key=lambda r: r[1])
    if verbose:
        print('   violating %d / %d (%.2f%%)  max %.3f  worst y=%.2f (%d angles)' % (
            total, samples, 100.0 * total / samples, max(r[2] for r in rows), worst[0], worst[1]))
        for r in sorted(rows, key=lambda r: -r[1])[:4]:
            print('   y=%.2f viol=%3d max=%.3f mean=%.3f' % r)
    return total, samples


def main():
    d1 = pmx.parse(O1)
    d6 = pmx.parse(O6)
    ab = load_card_table(CARD)
    src_scale = load_clothes_table(CLOTHES)
    print('clothing mods:', src_scale)

    # The effective transform difference between the current export at outfit N and outfit 1.
    def diff_table(coord):
        diff = {}
        for name, mods in ab.items():
            s = coord_scale(mods, coord)
            s1 = coord_scale(mods, 1)
            r = tuple(s[k] / s1[k] for k in range(3))
            if r != IDENT:
                diff[name] = r
        return diff

    card6 = diff_table(6)
    print('outfit6 vs outfit1 ABMX difference (%d bones):' % len(card6))
    for k, v in sorted(card6.items()):
        print('   %-24s %s' % (k, tuple(round(x, 4) for x in v)))
    print('entry lengths:', {n: len(m) for n, m in ab.items() if len(m) != 7})

    v1, t1 = garment_arrays(d1, 'skirt03')
    v6, t6 = garment_arrays(d6, 'skirt03')
    print('garment verts o1=%d o6=%d tris o1=%d o6=%d' % (len(v1), len(v6), len(t1), len(t6)))

    sim6 = skin(d1, v1, bone_matrices(d1, card6))
    p6 = d6['pos'][v6]
    if len(v1) == len(v6):
        err = np.linalg.norm(sim6 - p6, axis=1)
        print('validation vs actual outfit6: mean err %.5f  max %.5f  p99 %.5f' % (
            err.mean(), err.max(), np.percentile(err, 99)))
        try:
            from scipy.spatial import cKDTree
            dd, _ = cKDTree(p6).query(sim6)
            print('validation nearest: mean %.5f max %.5f p99 %.5f' % (
                dd.mean(), dd.max(), np.percentile(dd, 99)))
        except ImportError:
            pass
    else:
        print('vertex count differs, skip direct validation')

    _, _, body_tris, gar, gar_tris = fm.load(O1, 'body/o_body', 'skirt03')
    center = np.array([0.0, float(np.median(gar[:, 2]))])
    print('center', center)

    cases = [
        ('outfit1 base (no coordinate ABMX)', {}),
        ('game-faithful: source ABMX on cleared coordinate', src_scale),
        ('card outfit6 ABMX (current export)', card6),
        ('current export + source ABMX (no clear)', {**card6, **src_scale}),
    ]
    for name, scales in cases:
        pos = skin(d1, v1, bone_matrices(d1, scales))
        print('--', name)
        measure_tris(body_tris, pos[t1], center)


if __name__ == '__main__':
    main()
