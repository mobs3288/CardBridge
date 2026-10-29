"""Compares a texture CardBridge built through a game "create" material with the game's own render texture (sidecar dump).

    python -I -X utf8 compare_create_texture.py <truth folder> <textures folder of the CardBridge export> <renderer name> <create result prefix>
    e.g. ... truth2\\KK_138747 audit2\\KK_138747\\textures cf_Ohitomi_L02 cf_m_eye_create_result

The sidecar writes sRGB render textures linear, so the game's pixels are sRGB-encoded before comparing (mean absolute difference per channel, 0-255).
"""
import glob, json, sys
import numpy as np
from PIL import Image

def enc(x):
    x = x / 255.0
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.maximum(x, 0), 1 / 2.4) - 0.055) * 255

truth, textures, renderer, prefix = sys.argv[1:5]
for r in json.load(open(truth + '/renderers.json', encoding='utf-8')):
    if r['name'] == renderer and r['active']:
        t = r['materials'][0]['textures']['_MainTex']
        g = np.asarray(Image.open(truth + '/' + t['file']).convert('RGBA')).astype(float)
        break
else:
    sys.exit('renderer not found in the dump')
o = np.asarray(Image.open(sorted(glob.glob(textures + '/' + prefix + '*.png'))[0]).convert('RGBA').resize((g.shape[1], g.shape[0]))).astype(float)
ge = enc(g[..., :3])
m = (g[..., 3] > 10) & (o[..., 3] > 10)
print('game mean', ge[m].mean(0).round(1), 'ours', o[..., :3][m].mean(0).round(1), 'mean abs diff', np.abs(ge[m] - o[..., :3][m]).mean().round(2))
