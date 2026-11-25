# Diagnostic: rasterize the PMX skirt material front-on, sampling its texture alpha / colour, so the
# live viewer's "short, see-through skirt" can be compared with what the texture actually contains.
# usage: _skirt_alpharender.py <pmx> <texture.png> <out_prefix>
import struct, sys
from PIL import Image

def read_str(b, pos, enc):
    n = struct.unpack_from('<i', b, pos)[0]; pos += 4
    s = b[pos:pos+n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace'); pos += n
    return s, pos

def idx(b, pos, size):
    v = {1: b[pos], 2: struct.unpack_from('<H', b, pos)[0], 4: struct.unpack_from('<I', b, pos)[0]}[size]
    return v, pos + size

b = open(sys.argv[1], 'rb').read()
gcount = b[8]
enc, add, vis, tis, mis, bis, morphis, rbis = struct.unpack_from('<8B', b, 9)
pos = 9 + gcount
for _ in range(4): _, pos = read_str(b, pos, enc)
nv, = struct.unpack_from('<i', b, pos); pos += 4
vdata = []
for i in range(nv):
    x, y, z = struct.unpack_from('<3f', b, pos); pos += 12 + 12
    u, v = struct.unpack_from('<2f', b, pos); pos += 8
    pos += 16 * add
    wt = b[pos]; pos += 1
    if wt == 0: _, pos = idx(b, pos, bis)
    elif wt == 1: _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4
    elif wt in (2, 4):
        for _ in range(4): _, pos = idx(b, pos, bis)
        pos += 16
    elif wt == 3:
        _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4; pos += 36
    pos += 4
    vdata.append((x, y, z, u, v))
ni, = struct.unpack_from('<i', b, pos); pos += 4
faces = []
for i in range(0, ni, 3):
    a, pos = idx(b, pos, vis); c, pos = idx(b, pos, vis); d, pos = idx(b, pos, vis)
    faces.append((a, c, d))
nu, = struct.unpack_from('<i', b, pos); pos += 4
tex = []
for i in range(nu): t, pos = read_str(b, pos, enc); tex.append(t)
nm, = struct.unpack_from('<i', b, pos); pos += 4
mats = []
for i in range(nm):
    n, pos = read_str(b, pos, enc); ne, pos = read_str(b, pos, enc)
    diff = struct.unpack_from('<4f', b, pos); pos += 16
    pos += 12 + 4 + 12 + 1 + 16 + 4
    ti, pos = idx(b, pos, tis); si, pos = idx(b, pos, tis)
    pos += 1 + 1
    if b[pos-1] == 0: _, pos = idx(b, pos, tis)
    else: pos += 1
    memo, pos = read_str(b, pos, enc)
    cnt, = struct.unpack_from('<i', b, pos); pos += 4
    mats.append((n, ti, cnt))

img = Image.open(sys.argv[2]).convert('RGBA'); TW, TH = img.size; pix = img.load()
fstart = 0
target = None
for name, ti, cnt in mats:
    if 'bot_skirt' in name: target = (name, ti, cnt, fstart)
    fstart += cnt // 3
name, ti, cnt, fstart = target
tris = [faces[fstart + i] for i in range(cnt // 3)]
print(name, 'tris', len(tris), 'tex', tex[ti])

S = 700
xs = [vdata[v][0] for f in tris for v in f]; ys = [vdata[v][1] for f in tris for v in f]
x0, x1 = min(xs) - 0.02, max(xs) + 0.02
y0, y1 = min(ys) - 0.02, max(ys) + 0.02
def px(x): return (x - x0) / (x1 - x0) * (S - 1)
def py(y): return (1.0 - (y - y0) / (y1 - y0)) * (S - 1)
mask = Image.new('RGB', (S, S), (30, 30, 30))
texr = Image.new('RGB', (S, S), (30, 30, 30))
mp, tp = mask.load(), texr.load()
zbuf = [[-1e9] * S for _ in range(S)]
kept = 0
for f in tris:
    p = [vdata[i] for i in f]
    X = [px(v[0]) for v in p]; Y = [py(v[1]) for v in p]; Z = [v[2] for v in p]
    minx, maxx = max(0, int(min(X))), min(S - 1, int(max(X)) + 1)
    miny, maxy = max(0, int(min(Y))), min(S - 1, int(max(Y)) + 1)
    den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2])
    if abs(den) < 1e-9: continue
    for yy in range(miny, maxy + 1):
        for xx in range(minx, maxx + 1):
            a = ((Y[1] - Y[2]) * (xx - X[2]) + (X[2] - X[1]) * (yy - Y[2])) / den
            bb = ((Y[2] - Y[0]) * (xx - X[2]) + (X[0] - X[2]) * (yy - Y[2])) / den
            c = 1 - a - bb
            if a < -1e-6 or bb < -1e-6 or c < -1e-6: continue
            z = a * Z[0] + bb * Z[1] + c * Z[2]
            if z <= zbuf[yy][xx]: continue
            u = a * p[0][3] + bb * p[1][3] + c * p[2][3]
            v = a * p[0][4] + bb * p[1][4] + c * p[2][4]
            tx = min(TW - 1, max(0, int((u % 1.0) * TW))); ty = min(TH - 1, max(0, int((v % 1.0) * TH)))
            r, g, bl, al = pix[tx, ty]
            zbuf[yy][xx] = z
            v2 = 255 if al >= 128 else 0
            mp[xx, yy] = (v2, v2, v2)
            tp[xx, yy] = (r, g, bl)
            kept += 1
mask.save(sys.argv[3] + '_alphamask.png'); texr.save(sys.argv[3] + '_texcolour.png')
print('covered pixels', kept)
