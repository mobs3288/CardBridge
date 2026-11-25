"""Texture-mapped front render of an exported PMX to locate a black band artifact."""
import os, sys
import numpy as np
from PIL import Image
from _blackband_probe import parse

p = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\pepsi\OneDrive\Desktop\Yoko Littner (3)\Yoko_Littner.pmx'
base = os.path.join(os.path.dirname(p), 'textures')
d = parse(p)
pos, idx, mats, uvs, nrm = d['pos'], d['idx'], d['mats'], d['uv'], d['nrm']

# texture cache
texcache = {}
def tex(i):
    if i in texcache: return texcache[i]
    name = d['textures'][i]
    fp = os.path.join(base, os.path.basename(name))
    try:
        im = Image.open(fp).convert('RGBA')
        a = np.asarray(im, np.float32) / 255.0
    except Exception:
        a = np.array([[[1, 1, 1, 1]]], np.float32)
    texcache[i] = a
    return a

W, H = 700, 1400
xmin, xmax = pos[:, 0].min(), pos[:, 0].max()
ymin, ymax = pos[:, 1].min(), pos[:, 1].max()
s = min((W - 20) / (xmax - xmin), (H - 20) / (ymax - ymin))
ox = W / 2 - (xmin + xmax) / 2 * s
oy = H - 10 - ymax * s
screen = np.column_stack([pos[:, 0] * s + ox, H - (pos[:, 1] * s + oy)])

img = np.full((H, W, 3), 1.0, np.float32)
zbuf = np.full((H, W), 1e9, np.float32)
mid = np.full((H, W), -1, np.int32)

def bary(p0, p1, p2, x, y):
    dd = (p1[1]-p2[1])*(p0[0]-p2[0]) + (p2[0]-p1[0])*(p0[1]-p2[1])
    if abs(dd) < 1e-9: return None
    l0 = ((p1[1]-p2[1])*(x-p2[0]) + (p2[0]-p1[0])*(y-p2[1]))/dd
    l1 = ((p2[1]-p0[1])*(x-p2[0]) + (p0[0]-p2[0])*(y-p2[1]))/dd
    l2 = 1-l0-l1
    if l0 < -1e-6 or l1 < -1e-6 or l2 < -1e-6: return None
    return l0, l1, l2

hide = set(int(x) for x in (sys.argv[3].split(',') if len(sys.argv) > 3 else []) if x != '')
for mi, m in enumerate(mats):
    if mi in hide:
        continue
    t = tex(m['tex']) if 0 <= m['tex'] < len(d['textures']) else None
    th, tw = (t.shape[0], t.shape[1]) if t is not None else (1, 1)
    for fi in range(m['start'], m['end'], 3):
        a,b,c = idx[fi],idx[fi+1],idx[fi+2]
        p0,p1,p2 = screen[a],screen[b],screen[c]
        x0 = max(0, int(np.floor(min(p0[0],p1[0],p2[0]))))
        x1 = min(W-1, int(np.ceil(max(p0[0],p1[0],p2[0]))))
        y0 = max(0, int(np.floor(min(p0[1],p1[1],p2[1]))))
        y1 = min(H-1, int(np.ceil(max(p0[1],p1[1],p2[1]))))
        if x1 < x0 or y1 < y0: continue
        zz = (pos[a,2],pos[b,2],pos[c,2])
        uu = (uvs[a,0],uvs[b,0],uvs[c,0]); vv = (uvs[a,1],uvs[b,1],uvs[c,1])
        for y in range(y0,y1+1):
            for x in range(x0,x1+1):
                w = bary(p0,p1,p2,x+.5,y+.5)
                if w is None: continue
                z = w[0]*zz[0]+w[1]*zz[1]+w[2]*zz[2]
                if z >= zbuf[y,x]: continue
                if t is not None:
                    u = w[0]*uu[0]+w[1]*uu[1]+w[2]*uu[2]
                    v = w[0]*vv[0]+w[1]*vv[1]+w[2]*vv[2]
                    tx = min(tw-1, max(0, int(u*tw)))
                    ty = min(th-1, max(0, int(v*th)))
                    c4 = t[ty, tx]
                    if c4[3] < 0.5: continue
                    col = c4[:3]
                else:
                    col = m['diff'][:3]
                zbuf[y,x] = z
                img[y,x] = col
                mid[y,x] = mi

tag = '_hide' + sys.argv[3] if len(sys.argv) > 3 else ''
out = r'C:\Users\pepsi\OneDrive\Desktop\blackband_tex' + tag + '.png'
Image.fromarray((img*255).astype(np.uint8)).save(out)
rng = np.random.default_rng(3)
pal = np.vstack([rng.uniform(.1,1,(len(mats),3))])
idimg = (pal[np.clip(mid,0,None)]*255).astype(np.uint8)
Image.fromarray(idimg).save(r'C:\Users\pepsi\OneDrive\Desktop\blackband_material' + tag + '.png')
# report material ids in a horizontal strip at the waist (y around belt)
import collections
for y in range(380, 470, 10):
    row = mid[y, 250:450]
    cnt = collections.Counter(int(x) for x in row if x >= 0)
    print('y=',y, 'materials=', [(i, n, mats[i]['name']) for i,n in cnt.most_common(6)])
print('saved', out)
