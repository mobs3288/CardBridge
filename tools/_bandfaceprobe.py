"""Prototype: drop fully-clipped faces from an opaque PMX and render, to test a
geometry-level fix for the black waist band without making garments transparent."""
import os, sys
import numpy as np
from PIL import Image
from _blackband_probe import parse

p = sys.argv[1]                       # current opaque PMX (band present)
maskdir = sys.argv[2]                 # folder with binary-alpha textures from the cutout build
threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0   # fraction of clipped vertices needed
base = os.path.join(os.path.dirname(p), 'textures')
d = parse(p)
pos, idx, mats, uvs = d['pos'], d['idx'], d['mats'], d['uv']

texcache = {}
def tex(i, folder=base):
    k = (folder, i)
    if k in texcache: return texcache[k]
    name = os.path.basename(d['textures'][i])
    try:
        texcache[k] = np.asarray(Image.open(os.path.join(folder, name)).convert('RGBA'), np.float32) / 255.0
    except Exception:
        texcache[k] = np.array([[[1, 1, 1, 1]]], np.float32)
    return texcache[k]

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

dropped = 0
for mi, m in enumerate(mats):
    t = tex(m['tex']) if 0 <= m['tex'] < len(d['textures']) else None
    th, tw = (t.shape[0], t.shape[1]) if t is not None else (1, 1)
    # alpha mask from the cutout build, for the materials with real shader clips
    mk = tex(m['tex'], maskdir) if m['tex'] >= 0 and 0 <= m['tex'] < len(d['textures']) else None
    mh, mw = (mk.shape[0], mk.shape[1]) if mk is not None else (1, 1)
    for fi in range(m['start'], m['end'], 3):
        a,b,c = idx[fi],idx[fi+1],idx[fi+2]
        if mk is not None:
            vs = []
            for vi in (a,b,c):
                tx = min(mw-1, max(0, int(uvs[vi,0]*mw)))
                ty = min(mh-1, max(0, int(uvs[vi,1]*mh)))
                vs.append(mk[ty, tx, 3])
            if sum(1 for v in vs if v < 0.5) >= 3 * threshold:
                dropped += 1
                continue
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
                    col = t[ty, tx, :3]
                else:
                    col = m['diff'][:3]
                zbuf[y,x] = z
                img[y,x] = col
                mid[y,x] = mi

out = rf'C:\Users\pepsi\OneDrive\Desktop\band_faces_{threshold:.2f}.png'
Image.fromarray((img*255).astype(np.uint8)).save(out)
import collections
for y in range(380, 470, 10):
    row = mid[y, 250:450]
    cnt = collections.Counter(int(x) for x in row if x >= 0)
    print('y=',y, [(i,n,mats[i]['name']) for i,n in cnt.most_common(5)])
print('dropped', dropped, 'saved', out)
