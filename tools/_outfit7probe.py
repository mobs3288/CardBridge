import sys, os, collections
sys.path.insert(0, r'D:\Project K\cardbridge\tools')
import _blackband_probe as p
import numpy as np
from PIL import Image

base = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\pepsi\OneDrive\Desktop\Yoko OUTFIT7 CHECK'
d = p.parse(os.path.join(base, 'Yoko Littner.pmx'))
for i, m in enumerate(d['mats']):
    t = d['textures'][m['tex']] if 0 <= m['tex'] < len(d['textures']) else ''
    extra = ''
    if t:
        a = np.asarray(Image.open(os.path.join(base, 'textures', os.path.basename(t))).convert('RGBA'))[:, :, 3]
        c = collections.Counter(a.ravel().tolist())
        extra = 'A0=%d A255=%d mid=%d' % (c.get(0, 0), c.get(255, 0), sum(v for k, v in c.items() if 0 < k < 255))
    print('%2d %-44s a=%.2f %-44s %s' % (i, m['name'][:44], m['diff'][3], os.path.basename(t)[:44], extra))
