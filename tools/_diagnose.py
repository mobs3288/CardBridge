import sys
from collections import Counter

import numpy as np

sys.path.insert(0, 'tools')
import _pmxdeep as pmx

f = sys.argv[1] if len(sys.argv) > 1 else 'out/kk_custom/Oosuki_Mamako.pmx'
sub = sys.argv[2] if len(sys.argv) > 2 else 'skirt03'
d = pmx.parse(f)
bi = next(i for i, m in enumerate(d['mats']) if 'body/o_body' in m['name'])
gi = next(i for i, m in enumerate(d['mats']) if sub in m['name'])
bodyv = np.unique(d['mats'][bi]['verts'])
garv = np.unique(d['mats'][gi]['verts'])
body = d['pos'][bodyv]
gar = d['pos'][garv]
print('garment verts', len(gar), 'y min/max', gar[:, 1].min(), gar[:, 1].max())
hist, edges = np.histogram(gar[:, 1], bins=40)
for i, h in enumerate(hist):
    if h:
        print('  y %6.2f..%6.2f  %4d' % (edges[i], edges[i + 1], h))

print('body verts', len(body), 'y min/max', body[:, 1].min(), body[:, 1].max())
# dominance of body bones in the skirt y band
band = gar[(gar[:, 1] > gar[:, 1].max() - 4.0) & (gar[:, 1] < gar[:, 1].max())]
print('band y', band[:, 1].min(), band[:, 1].max())
# body verts in band and within radius 3 of x=0,z=median
center_z = float(np.median(band[:, 2]))
print('center_z', center_z)
sel = body[(body[:, 1] > 9.0) & (body[:, 1] < 13.5)]
r = np.sqrt(sel[:, 0] ** 2 + (sel[:, 2] - center_z) ** 2)
print('body band verts radial percentiles', np.percentile(r, [50, 90, 95, 99, 100]))
cnt = Counter()
for vi in bodyv:
    p = d['pos'][vi]
    if 9.0 < p[1] < 13.5 and np.sqrt(p[0] ** 2 + (p[2] - center_z) ** 2) > 2.6:
        for j in range(4):
            bn = int(d['bone'][vi, j])
            if bn >= 0 and d['weight'][vi, j] > 0.5:
                cnt[d['bones'][bn]['name']] += 1
print('dominant bones of body verts in band r>2.6:', cnt.most_common(20))
cnt2 = Counter()
for vi in bodyv:
    p = d['pos'][vi]
    if 9.0 < p[1] < 13.5:
        best = max(range(4), key=lambda j: d['weight'][vi, j] if d['bone'][vi, j] >= 0 else -1)
        bn = int(d['bone'][vi, best])
        if bn >= 0:
            cnt2[d['bones'][bn]['name']] += 1
print('dominant bones of body verts in band:', cnt2.most_common(20))
