import glob
import sys

sys.path.insert(0, 'tools')
import _pmxdeep as pmx

WANT = ['cf_d_sk_top', 'cf_d_sk_00_00', 'cf_d_sk_01_00', 'cf_d_sk_02_00', 'cf_d_sk_03_00', 'cf_d_sk_04_00',
        'cf_j_sk_00_00', 'cf_j_sk_00_02', 'cf_j_sk_02_02', 'cf_s_siri_L', 'cf_s_siri_R', 'cf_s_waist01',
        'cf_s_waist02', 'cf_s_thigh01_L', 'cf_s_thigh01_R', 'cf_j_waist01', 'cf_j_waist02', 'cf_s_head']

paths = ['out/kk_custom/Oosuki_Mamako.pmx'] + sorted(glob.glob('out/kk_ctrl_*/*.pmx')) + [
    'out/kk_outfit1/Oosuki_Mamako.pmx', 'out/kk_outfit2/Oosuki_Mamako.pmx']
for p in paths:
    d = pmx.parse(p)
    byname = {b['name']: b for b in d['bones']}
    name = p.replace('\\', '/').split('/')[-2]
    print('%-28s' % name, end='')
    for n in ('cf_j_waist01', 'cf_s_head'):
        b = byname.get(n)
        print('  %s=(%.2f,%.2f,%.2f)' % (n, *b['pos']) if b else '  %s=-' % n, end='')
    print()
    for n in WANT:
        b = byname.get(n)
        if b:
            print('   %-16s (%7.3f,%7.3f,%7.3f) parent=%s' % (
                n, b['pos'][0], b['pos'][1], b['pos'][2],
                d['bones'][b['parent']]['name'] if 0 <= b['parent'] < len(d['bones']) else '-'))
