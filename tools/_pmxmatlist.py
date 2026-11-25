import struct, sys

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
pos += nv * (12 + 12 + 8 + 16*add + 1 + (bis if True else 0))
# skip vertices properly
pos = 9 + gcount
for _ in range(4): _, pos = read_str(b, pos, enc)
nv, = struct.unpack_from('<i', b, pos); pos += 4
for i in range(nv):
    pos += 12 + 12 + 8 + 16 * add
    wt = b[pos]; pos += 1
    if wt == 0: _, pos = idx(b, pos, bis)
    elif wt == 1: _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4
    elif wt in (2, 4):
        for _ in range(4): _, pos = idx(b, pos, bis)
        pos += 16
    elif wt == 3:
        _, pos = idx(b, pos, bis); _, pos = idx(b, pos, bis); pos += 4; pos += 36
    pos += 4
ni, = struct.unpack_from('<i', b, pos); pos += 4
pos += ni * vis
nu, = struct.unpack_from('<i', b, pos); pos += 4
tex = []
for i in range(nu): t, pos = read_str(b, pos, enc); tex.append(t)
nm, = struct.unpack_from('<i', b, pos); pos += 4
for i in range(nm):
    n, pos = read_str(b, pos, enc); ne, pos = read_str(b, pos, enc)
    diff = struct.unpack_from('<4f', b, pos); pos += 16
    pos += 12 + 4 + 12 + 1 + 16 + 4
    ti, pos = idx(b, pos, tis); si, pos = idx(b, pos, tis)
    pos += 1
    tflag = b[pos]; pos += 1
    if tflag == 0: _, pos = idx(b, pos, tis)
    else: pos += 1
    memo, pos = read_str(b, pos, enc)
    cnt, = struct.unpack_from('<i', b, pos); pos += 4
    print(f'{i:2d} tris={cnt//3:6d} alpha={diff[3]:.3f} tex={ti:2d} {tex[ti] if ti < len(tex) else "?"}  name={n}')
