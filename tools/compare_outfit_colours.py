"""Mean colour of every worn garment's main texture: CardBridge's PMX texture against the game's _MainTex (sidecar dump).

    python -I -X utf8 compare_outfit_colours.py <truth folder> <export folder with the .pmx and textures\\>

The game's render textures are dumped linear, so they are sRGB-encoded first. Ours is the texture the PMX material points at.
"""
import glob, json, os, re, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'audit'))
import pmxinfo

def enc(x):
    x = x / 255.0
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.maximum(x, 0), 1 / 2.4) - 0.055) * 255

truth, export = sys.argv[1:3]
m = pmxinfo.load(glob.glob(export + '/*.pmx')[0])
ours = {}
for mat in m['M']:
    if mat['tex'] >= 0 and mat['name'].startswith('outfit'):
        part = mat['name'].split('/')[0].split('.')[-1]  # outfit0.acc33/... -> acc33
        ours[(part if part.startswith('acc') else '') + mat['name'].split('/')[-1].split('#')[0]] = os.path.join(export, m['T'][mat['tex']].replace('/', os.sep))
rows = []
for r in json.load(open(truth + '/renderers.json', encoding='utf-8')):
    name = r['name'][4:-1] if r['name'].startswith('CTA_') and r['name'].endswith('_') else r['name']  # AccessoryClothes clones
    slot = re.search(r'/ca_slot(\d+)/', r['path'])
    if slot: name = 'acc%02d' % int(slot.group(1)) + name
    if not (r['active'] and r['enabled']) or name not in ours: continue
    t = (r['materials'][0] or {}).get('textures', {}).get('_MainTex')
    if not t or not t.get('file'): continue
    g = np.asarray(Image.open(truth + '/' + t['file']).convert('RGBA')).astype(float)
    o = np.asarray(Image.open(ours[name]).convert('RGBA').resize((g.shape[1], g.shape[0]))).astype(float)
    mk = (g[..., 3] > 200) & (o[..., 3] > 200)
    if mk.sum() < 100: continue
    gm = enc(g[..., :3])
    mat0 = r['materials'][0]
    mt = mat0['textures'].get('_ColorMask')
    if name.startswith('acc') and mt and mt.get('file'):
        # An accessory's main texture is dumped untinted (the game's shader colours it by the mask); tint it the way CardBridge bakes it.
        k = np.asarray(Image.open(truth + '/' + mt['file']).convert('RGBA').resize((g.shape[1], g.shape[0]))).astype(float)[..., :3] / 255.0
        c = [np.array(mat0['colors'].get(n, [1, 1, 1, 1])[:3]) for n in ('_Color', '_Color2', '_Color3')]
        t = 1.0 + k[..., 0:1] * (c[0] - 1.0)
        t = t + k[..., 1:2] * (c[1] - t)
        t = t + k[..., 2:3] * (c[2] - t)
        gm = gm * t
    ge = gm[mk].mean(0)
    oe = o[..., :3][mk].mean(0)
    rows.append((float(np.abs(ge - oe).max()), name, ge.round(0).tolist(), oe.round(0).tolist()))
rows.sort(reverse=True)
for d, n, a, b in rows:
    print('%5.1f  %-26s game %s ours %s' % (d, n, a, b))
print(len(rows), 'garment textures compared')
