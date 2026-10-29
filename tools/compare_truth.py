"""Compares a CardBridge PMX against the ground truth the in-game sidecar dumped (cardbridge/sidecar), renderer by renderer.

    python -I -X utf8 compare_truth.py <truth folder for one card> <model.pmx> [pmxinfo module folder]

Truth units are metres, the PMX is scaled by 12.5 (manifest export.scale). For every renderer the game drew, the PMX material
with the same mesh name and the closest centroid is paired, and the two vertex clouds are compared: centroid offset, size ratio
per axis and mean nearest-vertex distance. Prints the worst pairs first. Development tool: it needs numpy.
"""
import json, os, struct, sys
import numpy as np

SCALE = 12.5


def load_truth(folder):
    renderers = json.load(open(os.path.join(folder, 'renderers.json'), encoding='utf-8'))
    out = []
    for r in renderers:
        f = r.get('meshFile')
        if not f or not r.get('enabled') or not r.get('active'):
            continue
        b = open(os.path.join(folder, f), 'rb').read()
        n = struct.unpack_from('<i', b, 0)[0]
        pts = np.frombuffer(b, dtype='<f4', count=n * 3, offset=4).reshape(n, 3).astype(np.float64)
        out.append((r, pts))
    return out


def load_pmx_materials(path):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'audit'))
    import pmxinfo  # the small reader kept beside the audit outputs
    m = pmxinfo.load(path)
    V = np.array([v[0] for v in m['V']], dtype=np.float64)
    F = np.array(m['F'], dtype=np.int64)
    mats, o = [], 0
    for mat in m['M']:
        idx = F[o:o + mat['cnt']]
        o += mat['cnt']
        if len(idx):
            mats.append((mat['name'], V[np.unique(idx)] / SCALE))
    return mats


def plain_name(name):
    # AccessoryClothes clones a garment into an accessory as CTA_<renderer>_; CardBridge keeps the garment's own name
    return name[4:-1] if name.startswith('CTA_') and name.endswith('_') else name


def nearest_mean(a, b, cap=4000):
    if len(a) > cap: a = a[np.linspace(0, len(a) - 1, cap).astype(int)]
    if len(b) > cap: b = b[np.linspace(0, len(b) - 1, cap).astype(int)]
    d = np.sqrt(((a[:, None, :] - b[None, :, :]) ** 2).sum(-1))
    return float(d.min(1).mean())


def main():
    truth = load_truth(sys.argv[1])
    mats = load_pmx_materials(sys.argv[2])
    used = set()
    rows = []
    missing = []
    for r, pts in truth:
        name = plain_name(r['name'])
        cands = [(i, n, p) for i, (n, p) in enumerate(mats) if i not in used and n.split('/', 1)[-1].split('#')[0] == name]
        if not cands:
            missing.append((name, len(pts)))
            continue
        c = pts.mean(0)
        i, n, p = min(cands, key=lambda t: np.linalg.norm(t[2].mean(0) - c))
        used.add(i)
        dc = np.linalg.norm(p.mean(0) - c)
        sz_t, sz_p = pts.max(0) - pts.min(0), p.max(0) - p.min(0)
        ratio = float(np.max(np.abs(np.log((sz_p + 1e-6) / (sz_t + 1e-6)))))
        rows.append((dc, ratio, nearest_mean(p, pts), name, n, len(pts), len(p)))
    rows.sort(reverse=True)
    print('paired %d of %d game renderers; %d game renderers have no CardBridge mesh' % (len(rows), len(truth), len(missing)))
    print('%8s %8s %8s  %-28s %s' % ('centroid', 'size', 'meanNN', 'renderer', 'pmx material (verts game/ours)'))
    for dc, ratio, nn, name, n, a, b in rows[:25]:
        print('%7.1fcm %7.0f%% %7.1fcm  %-28s %s (%d/%d)' % (dc * 100, (np.exp(ratio) - 1) * 100, nn * 100, name[:28], n, a, b))
    if missing:
        print('missing:', ', '.join('%s(%d)' % m for m in missing[:30]))
    extra = [n for i, (n, p) in enumerate(mats) if i not in used]
    if extra:
        print('CardBridge-only materials:', ', '.join(extra[:30]))


if __name__ == '__main__':
    main()
