"""Geometry error of two exports of the same cards against the game dump, side by side (development tool).

    python -I -X utf8 compare_modes.py <truth root> <export root A> <export root B> <card> [<card> ...]

For every card prints, per export, the mean nearest-vertex distance over all paired renderers (the body and bra rows are left out: CardBridge
removes the triangles clothes hide) and the number of renderers above 2 cm. Used to judge a change such as the DynamicBone settle.
"""
import glob, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'audit'))
import compare_truth as ct


def score(truth, pmx):
    mats = ct.load_pmx_materials(pmx)
    used, errs = set(), []
    for r, pts in truth:
        name = ct.plain_name(r['name'])
        if name in ('o_body_a', 'o_bra_a'):
            continue
        cands = [(i, n, p) for i, (n, p) in enumerate(mats) if i not in used and n.split('/', 1)[-1].split('#')[0] == name]
        if not cands:
            continue
        c = pts.mean(0)
        i, n, p = min(cands, key=lambda t: np.linalg.norm(t[2].mean(0) - c))
        used.add(i)
        errs.append(ct.nearest_mean(p, pts) * 100)
    return np.array(errs)


def main():
    truth_root, a_root, b_root = sys.argv[1:4]
    for card in sys.argv[4:]:
        truth = ct.load_truth(os.path.join(truth_root, card))
        line = '%-34s' % card
        for root in (a_root, b_root):
            pmx = glob.glob(os.path.join(root, card, '*.pmx'))
            if not pmx:
                line += '   (no export)'
                continue
            e = score(truth, pmx[0])
            line += '   mean %5.2f cm  >2cm: %2d of %3d' % (e.mean(), (e > 2).sum(), len(e))
        print(line)


if __name__ == '__main__':
    main()
