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
texnames=[]
for i in range(nu): t, pos = read_str(b, pos, enc); texnames.append(t)
nm, = struct.unpack_from('<i', b, pos); pos += 4
mats = []
for i in range(nm):
    n, pos = read_str(b, pos, enc); ne, pos = read_str(b, pos, enc)
    diff = struct.unpack_from('<4f', b, pos); pos += 16
    pos += 12 + 4 + 12
    pos += 1 + 16 + 4
    ti, pos = idx(b, pos, tis); si, pos = idx(b, pos, tis)
    pos += 1
    tflag = b[pos]; pos += 1
    if tflag == 0: _, pos = idx(b, pos, tis)
    else: pos += 1
    memo, pos = read_str(b, pos, enc)
    cnt, = struct.unpack_from('<i', b, pos); pos += 4
    mats.append((n, ti, cnt, diff[3]))

img = Image.open(sys.argv[2]).convert('RGBA')
W, H = img.size
pix = img.load()
fstart = 0
for name, ti, cnt, alpha in mats:
    if 'bot_skirt' in name or 'inner_a' in name:
        tris = cnt // 3
        bands = {}
        for f in faces[fstart:fstart+tris]:
            ys = [vdata[vi][1] for vi in f]
            us = [vdata[vi][3] for vi in f]
            vs = [vdata[vi][4] for vi in f]
            u = (sum(us) / 3.0) % 1.0
            v = (sum(vs) / 3.0) % 1.0
            x = min(W-1, max(0, int(u * W)))
            yA = min(H-1, max(0, int((1.0 - v) * H)))
            yB = min(H-1, max(0, int(v * H)))
            band = int(min(ys) // 2) * 2
            d = bands.setdefault(band, [0,0,0,0,0,0,0,0,0])  # tot, cutA, cutB, aSumA, aSumB, nA, nB
            d[0] += 1
            for arr, yy in ((1, yA), (2, yB)):
                a = pix[x, yy][3]
                if a < 128: d[arr] += 1
                d[arr+2] += a
            # per-vertex alpha for convention B (v*H), count any-vertex-zero
            zero = 0
            for vi in f:
                uu = vdata[vi][3] % 1.0; vv = vdata[vi][4] % 1.0
                xx = min(W-1, max(0, int(uu * W))); yy = min(H-1, max(0, int(vv * H)))
                if pix[xx, yy][3] < 128: zero += 1
            d[6] += zero
            d[7] += 3
        print(name, 'tris', tris, 'tex', ti, texnames[ti] if ti < len(texnames) else '?', 'matAlpha', alpha)
        print('  band            tris  cut(1-v)  cut(v)  meanA(1-v) meanA(v)  verts<128')
        for band in sorted(bands):
            tot, cutA, cutB, aA, aB, _, zero, verts, _ = bands[band]
            print(f'  y [{band:5d},{band+2:5d}) {tot:6d} {cutA*100.0/tot:8.1f}% {cutB*100.0/tot:7.1f}% {aA/tot:10.1f} {aB/tot:8.1f} {zero*100.0/verts:9.1f}%')
    fstart += cnt // 3
