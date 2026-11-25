"""Diagnostic: print PMX material diffuse/texture/shading flags (subset).

Usage: python tools/_pmxmatdetails.py <file.pmx> [substring]
"""
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')


def read_str(b, pos, enc):
    n = struct.unpack_from('<i', b, pos)[0]; pos += 4
    return b[pos:pos+n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace'), pos+n


def idx(b, pos, size):
    v = {1: b[pos], 2: struct.unpack_from('<H', b, pos)[0], 4: struct.unpack_from('<I', b, pos)[0]}[size]
    return v, pos + size


b = open(sys.argv[1], 'rb').read()
enc, add, vis, tis, mis, bis, morphis, rbis = struct.unpack_from('<8B', b, 9)
pos = 9 + b[8]
for _ in range(4):
    _, pos = read_str(b, pos, enc)
nv, = struct.unpack_from('<i', b, pos); pos += 4
for _ in range(nv):
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
for _ in range(nu):
    _, pos = read_str(b, pos, enc)
nm, = struct.unpack_from('<i', b, pos); pos += 4
for _ in range(nm):
    n, pos = read_str(b, pos, enc); _, pos = read_str(b, pos, enc)
    diff = struct.unpack_from('<4f', b, pos); pos += 16
    spec = struct.unpack_from('<3f', b, pos); pos += 12
    spow, = struct.unpack_from('<f', b, pos); pos += 4
    amb = struct.unpack_from('<3f', b, pos); pos += 12
    flags = b[pos]; pos += 1
    pos += 16 + 4
    ti, pos = idx(b, pos, tis); si, pos = idx(b, pos, tis)
    mode = b[pos]; pos += 1
    env = b[pos]; pos += 1
    if mode == 0:
        _, pos = idx(b, pos, tis)
    else:
        pos += 1
    _, pos = read_str(b, pos, enc)
    cnt, = struct.unpack_from('<i', b, pos); pos += 4
    if len(sys.argv) < 3 or sys.argv[2] in n:
        print(f'{n}: diffuse=({diff[0]:.3f},{diff[1]:.3f},{diff[2]:.3f},{diff[3]:.3f}) '
              f'spec=({spec[0]:.2f},{spec[1]:.2f},{spec[2]:.2f}) spow={spow:.2f} '
              f'amb=({amb[0]:.2f},{amb[1]:.2f},{amb[2]:.2f}) flags=0x{flags:02x} '
              f'tex={ti} sph={si}/{mode}/{env} tris={cnt//3}')
