"""Scratch: measure the effective transform between the outfit1 and outfit6 skirt exports."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _pmxdeep as pmx

O1 = 'out/kk_custom_o1/Oosuki_Mamako.pmx'
O6 = 'out/kk_custom/Oosuki_Mamako.pmx'


def garment(d, sub='skirt03'):
    gi = next(i for i, m in enumerate(d['mats']) if sub in m['name'])
    idx = d['mats'][gi]['verts']
    vidx = np.unique(idx)
    return vidx, idx.reshape(-1, 3)


def main():
    d1 = pmx.parse(O1)
    d6 = pmx.parse(O6)
    v1, t1 = garment(d1)
    v6, t6 = garment(d6)
    print('verts', len(v1), len(v6), 'same indices', np.array_equal(v1, v6))
    p1 = d1['pos'][v1]
    p6 = d6['pos'][v6]
    # fit p6 = a*p1 + b per axis, all verts
    for axis, name in enumerate('xyz'):
        a, b = np.polyfit(p1[:, axis], p6[:, axis], 1)
        pred = a * p1[:, axis] + b
        print('axis %s: a=%.6f b=%.6f center=%.4f resid mean=%.5f max=%.5f' % (
            name, a, b, -b / a if a else 0, np.abs(pred - p6[:, axis]).mean(), np.abs(pred - p6[:, axis]).max()))
    # per dominant-bone fit on x only and xz
    names = [b['name'] for b in d6['bones']]
    dom = []
    for vi in v6:
        best = -1
        bj = -1
        for j in range(4):
            bn = int(d6['bone'][vi, j])
            w = float(d6['weight'][vi, j])
            if bn >= 0 and w > best:
                best = w
                bj = bn
        dom.append(names[bj] if 0 <= bj < len(names) else '?')
    from collections import Counter
    cnt = Counter(dom)
    print('dominant bones:', cnt)
    for bone, n in cnt.most_common():
        mask = np.array([x == bone for x in dom])
        if n < 20:
            continue
        a, b = np.polyfit(p1[mask, 0], p6[mask, 0], 1)
        c, e = np.polyfit(p1[mask, 2], p6[mask, 2], 1)
        print('%-18s n=%4d  x a=%.5f c=%.4f  z a=%.5f c=%.4f' % (bone, n, a, -b / a if a else 0, c, -e / c if c else 0))
    # weights summary
    wmax = d6['weight'][v6].max(axis=1)
    wsum = d6['weight'][v6].sum(axis=1)
    print('weight max percentiles', np.percentile(wmax, [1, 5, 50, 95, 100]))
    print('weight sum percentiles', np.percentile(wsum, [1, 5, 50, 95, 100]))
    print('weight types', Counter(d6['wtype'][v6].tolist()))


if __name__ == '__main__':
    main()
