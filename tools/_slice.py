import sys

import numpy as np

sys.path.insert(0, 'tools')
import _fitmetric as fm

path = sys.argv[1] if len(sys.argv) > 1 else 'out/kk_custom/Oosuki_Mamako.pmx'
ys = [float(x) for x in sys.argv[2:]] or [12.31]
d, body, body_tris, gar, gar_tris = fm.load(path, 'body/o_body', 'skirt03')
center = np.array([0.0, float(np.median(body[(body[:, 1] > gar[:, 1].min()) & (body[:, 1] < gar[:, 1].max())][:, 2]))])
angles = np.linspace(0, 2 * np.pi, 72, endpoint=False)
for yp in ys:
    rbs = fm.ray_radii(body_tris, yp, center, angles)
    rgs = fm.ray_radii(gar_tris, yp, center, angles)
    print('y=%.2f center=%s' % (yp, center))
    print('deg   garment  body  delta  dir')
    for ai, a in enumerate(angles):
        rg = rgs[ai][-1] if len(rgs[ai]) else float('nan')
        rb = rbs[ai][-1] if len(rbs[ai]) else float('nan')
        deg = np.degrees(a)
        if np.isfinite(rg):
            print('%5.0f  %7.3f %6.3f %6.3f  %s' % (deg, rg, rb, rb - rg, 'FRONT' if 225 < deg < 315 else ('BACK' if 45 < deg < 135 else '')))
