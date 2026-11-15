"""Scratch: measure body-vs-garment protrusion in an exported PMX.

For horizontal slices through the garment, cast rays outward from the pelvis axis and compare the
outermost body radius with the outermost garment radius. A sample where the body is farther out than
the garment is body geometry that pokes through the garment (looks like clipping).

Only body triangles skinned to the pelvis / upper legs are considered, so the arms and hands of the
A-pose do not count as "body outside the skirt".
"""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _pmxdeep as pmx

PELVIS = ('waist', 'siri', 'thigh', 'kokan', 'spine', 'leg', 'hip', '下半身', '上半身')


def dominant_names(d, vis):
    out = []
    for vi in vis:
        best = -1
        bj = -1
        for j in range(4):
            bn = int(d['bone'][vi, j])
            w = float(d['weight'][vi, j])
            if bn >= 0 and w > best:
                best = w
                bj = bn
        out.append(d['bones'][bj]['name'] if 0 <= bj < len(d['bones']) else '')
    return out


def load(path, body_sub, garment_sub):
    d = pmx.parse(path)
    bi = next(i for i, m in enumerate(d['mats']) if body_sub in m['name'])
    gi = next(i for i, m in enumerate(d['mats']) if garment_sub in m['name'])
    bidx = np.unique(d['mats'][bi]['verts'])
    gidx = np.unique(d['mats'][gi]['verts'])
    body_pos = d['pos'][bidx]
    gar_pos = d['pos'][gidx]
    names = dominant_names(d, bidx)
    pelvis = np.array([any(k in n for k in PELVIS) for n in names], bool)
    pelvis_idx = set(int(v) for v, ok in zip(bidx, pelvis) if ok)
    tris_all = d['mats'][bi]['verts'].reshape(-1, 3)
    keep = np.array([all(int(v) in pelvis_idx for v in t) for t in tris_all], bool)
    body_tris = d['pos'][tris_all[keep]]
    gar_tris = d['pos'][d['mats'][gi]['verts'].reshape(-1, 3)]
    return d, body_pos[pelvis], body_tris, gar_pos, gar_tris


def cross2(a, b):
    return a[..., 0] * b[..., 1] - a[..., 1] * b[..., 0]


def cut_segments(tris, yp):
    """Cut a triangle soup with the plane y=yp; return two (N,2) point arrays in xz."""
    n = len(tris)
    pts = np.full((n, 2, 2), np.nan)
    cnt = np.zeros(n, np.uint8)
    a, b, c = tris[:, 0, :], tris[:, 1, :], tris[:, 2, :]
    for p, q in ((a, b), (b, c), (c, a)):
        dy = q[:, 1] - p[:, 1]
        ok = np.abs(dy) > 1e-9
        t = np.zeros(n)
        t[ok] = (yp - p[ok, 1]) / dy[ok]
        ok &= (t >= 0.0) & (t <= 1.0) & (cnt < 2)
        idx = np.nonzero(ok)[0]
        if len(idx) == 0:
            continue
        slots = cnt[idx]
        pts[idx, slots, 0] = p[idx, 0] + (q[idx, 0] - p[idx, 0]) * t[idx]
        pts[idx, slots, 1] = p[idx, 2] + (q[idx, 2] - p[idx, 2]) * t[idx]
        cnt[idx] += 1
    sel = np.nonzero(cnt >= 2)[0]
    return pts[sel, 0, :], pts[sel, 1, :]


def ray_radii(tris, yp, center, angles, eps=1e-9):
    """For each angle, all radii where the ray from center hits the mesh at plane yp (A, hits)."""
    P, Q = cut_segments(tris, yp)
    if len(P) == 0:
        return np.zeros((len(angles), 0))
    P = P - center
    e = (Q - center) - P
    d = np.stack([np.cos(angles), np.sin(angles)], axis=1)   # (A,2)
    de = cross2(d[:, None, :], e[None, :, :])                 # (A,N)
    pe = cross2(P[None, :, :], e[None, :, :])
    pd = cross2(P[None, :, :], np.broadcast_to(d[:, None, :], (len(angles), len(P), 2)))
    with np.errstate(divide='ignore', invalid='ignore'):
        r = pe / de
        s = pd / de
    ok = (np.abs(de) > eps) & (r > 0) & (s >= -1e-6) & (s <= 1 + 1e-6)
    out = []
    for ai in range(len(angles)):
        rr = np.sort(r[ai][ok[ai]])
        out.append(rr)
    return out


def ray_max_radius(tris, yp, center, angles, eps=1e-9):
    out = np.full(len(angles), np.nan)
    for ai, rr in enumerate(ray_radii(tris, yp, center, angles, eps)):
        if len(rr):
            out[ai] = rr[-1]
    return out


def measure(path, body_sub='body/o_body', garment_sub='skirt', angles=720, slices=100, cap=0.5,
            verbose=True):
    d, body, body_tris, gar, gar_tris = load(path, body_sub, garment_sub)
    y0, y1 = gar[:, 1].min(), gar[:, 1].max()
    span = y1 - y0
    ys = np.linspace(y0 + span * 0.03, y1 - span * 0.02, slices)
    band = body[(body[:, 1] > y0) & (body[:, 1] < y1)]
    center = np.array([0.0, float(np.median(band[:, 2]))])
    th = np.linspace(0, 2 * np.pi, angles, endpoint=False)
    rows = []
    for yp in ys:
        rbs = ray_radii(body_tris, yp, center, th)
        rgs = ray_radii(gar_tris, yp, center, th)
        rb = np.array([rr[-1] if len(rr) else np.nan for rr in rbs])
        rg = np.array([rr[-1] if len(rr) else np.nan for rr in rgs])
        # body point just outside the outermost garment surface, within cap
        near = np.full(len(th), np.nan)
        for ai in range(len(th)):
            if not np.isfinite(rg[ai]):
                continue
            cand = rbs[ai][(rbs[ai] <= rg[ai] + cap)]
            if len(cand):
                near[ai] = cand[-1]
        viol = np.isfinite(rg) & np.isfinite(near) & (near > rg + 0.005)
        dlt = np.where(viol, near - rg, np.nan)
        rows.append((yp, int(viol.sum()),
                     float(np.nanmax(dlt)) if viol.any() else 0.0,
                     float(np.nanmean(dlt)) if viol.any() else 0.0))
    total = sum(r[1] for r in rows)
    samples = len(rows) * angles
    worst = max(rows, key=lambda r: r[1])
    maxp = max(r[2] for r in rows)
    if verbose:
        print('%-28s tris body=%6d cloth=%6d  y=%.2f..%.2f center_z=%.2f' % (
            path.replace('\\', '/').split('/')[-2], len(body_tris), len(gar_tris), y0, y1, center[1]))
        print('   violating samples %d / %d (%.2f%%)  max protrusion %.3f  worst y=%.2f (%d angles)' % (
            total, samples, 100.0 * total / samples, maxp, worst[0], worst[1]))
        top = sorted(rows, key=lambda r: -r[1])[:5]
        for r in top:
            print('   y=%.2f  viol=%3d  max=%.3f mean=%.3f' % r)
    return {'path': path, 'rows': rows, 'total': total, 'samples': samples, 'max': maxp,
            'body': body, 'body_tris': body_tris, 'garment': gar, 'ymin': y0, 'ymax': y1,
            'center': center}


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        args = ['out/kk_custom/Oosuki_Mamako.pmx', 'out/kk_outfit1/Oosuki_Mamako.pmx',
                'out/kk_outfit2/Oosuki_Mamako.pmx']
    for p in args:
        sub = 'pants' if 'outfit2' in p else ('skirt02' if 'outfit1' in p else 'skirt03')
        try:
            measure(p, garment_sub=sub)
        except Exception as e:
            print(p, 'ERR', repr(e))
