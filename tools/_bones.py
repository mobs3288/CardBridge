import glob
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _pmxdeep as pmx

WANT = ['cf_d_sk_top', 'cf_d_sk_00_00', 'cf_d_sk_01_00', 'cf_d_sk_02_00', 'cf_d_sk_03_00', 'cf_d_sk_04_00',
        'cf_d_sk_05_00', 'cf_d_sk_06_00', 'cf_d_sk_07_00', 'cf_j_sk_00_00', 'cf_j_sk_00_01', 'cf_j_sk_00_02',
        'cf_j_sk_01_02', 'cf_j_sk_02_02', 'cf_s_siri_L', 'cf_s_siri_R', 'cf_s_waist01', 'cf_s_waist02',
        'cf_s_thigh01_L', 'cf_s_thigh01_R', 'cf_j_waist01', 'cf_j_waist02', ' cf_s_spine01', 'cf_s_spine01',
        'cf_s_head', 'cf_j_head', 'cf_n_height']


def world(d):
    byname = {b['name']: i for i, b in enumerate(d['bones'])}
    out = {}
    memo = {}

    def w(i):
        if i in memo:
            return memo[i]
        b = d['bones'][i]
        p = np.array(b['pos'], float)
        if 0 <= b['parent'] < len(d['bones']):
            p = p + w(b['parent'])
        memo[i] = p
        return p

    for n in WANT:
        if n in byname:
            out[n] = w(byname[n])
    # height: highest bone y (head top generally)
    out['_maxbone'] = max(w(i)[1] for i in range(len(d['bones'])))
    return out


paths = ['out/kk_custom/Oosuki_Mamako.pmx'] + sorted(glob.glob('out/kk_ctrl_*/*.pmx')) + [
    'out/kk_outfit1/Oosuki_Mamako.pmx', 'out/kk_outfit2/Oosuki_Mamako.pmx']
for p in paths:
    d = pmx.parse(p)
    w = world(d)
    top = w['_maxbone']
    print('%-30s top=%.2f' % (p.replace('\\', '/').split('/')[-2], top))
    for n in WANT:
        if n in w:
            v = w[n]
            print('   %-18s %7.3f %7.3f %7.3f' % (n, v[0], v[1], v[2]))
