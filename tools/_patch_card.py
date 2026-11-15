"""Scratch: patch a Koikatsu card's ABMX table (investigation only).

Rebuilds the card's block container with an edited KKABMPlugin.ABMData payload so the real
converter can be run against coordinate-table variants.
"""
import io
import struct
import sys

import msgpack

try:
    import lz4.block
except ImportError:
    lz4 = None


def read_dotnet_string(b, o):
    n = 0
    sh = 0
    while True:
        c = b[o]
        o += 1
        n |= (c & 0x7F) << sh
        if not c & 0x80:
            break
        sh += 7
    return b[o:o + n], o + n


def parse_container(path):
    b = open(path, 'rb').read()
    o = 8
    while True:
        ln, = struct.unpack('>I', b[o:o + 4])
        typ = b[o + 4:o + 8]
        o += 12 + ln
        if typ == b'IEND':
            break
    o += 4  # productNo
    marker, o = read_dotnet_string(b, o)
    version, o = read_dotnet_string(b, o)
    faceLen, = struct.unpack('<i', b[o:o + 4])
    o += 4 + faceLen
    hdr_len_off = o
    hdrLen, = struct.unpack('<i', b[o:o + 4])
    o += 4
    hdr = msgpack.unpackb(b[o:o + hdrLen], raw=False, strict_map_key=False)
    o += hdrLen
    dataSize, = struct.unpack('<q', b[o:o + 8])
    o += 8
    data = b[o:o + dataSize]
    if o + dataSize != len(b):
        print('warning: %d trailing bytes' % (len(b) - o - dataSize))
    blocks = []
    for info in hdr['lstInfo']:
        blocks.append((info['name'], data[info['pos']:info['pos'] + info['size']]))
    return b[:hdr_len_off], hdr, blocks, marker, version


def rebuild(prefix, hdr, blocks, path):
    pos = 0
    infos = []
    datas = []
    for info, (name, block) in zip(hdr['lstInfo'], blocks):
        assert info['name'] == name
        entry = dict(info)
        entry['pos'] = pos
        entry['size'] = len(block)
        infos.append(entry)
        datas.append(block)
        pos += len(block)
    hdr = dict(hdr)
    hdr['lstInfo'] = infos
    hdrBytes = msgpack.packb(hdr, use_single_float=True)
    data = b''.join(datas)
    out = bytearray(prefix)
    out += struct.pack('<i', len(hdrBytes))
    out += hdrBytes
    out += struct.pack('<q', len(data))
    out += data
    open(path, 'wb').write(bytes(out))
    print('wrote', path, len(out), 'bytes')


def decode_bin(bd):
    if bd[0] in (0xC7, 0xC8, 0xC9):
        lb = {0xC7: 1, 0xC8: 2, 0xC9: 4}[bd[0]]
        assert bd[1 + lb] == 99
        o = 1 + lb + 1
        un = msgpack.Unpacker(io.BytesIO(bd[o:]), raw=False, strict_map_key=False)
        ulen = un.unpack()
        o += un.tell()
        raw = lz4.block.decompress(bd[o:], uncompressed_size=ulen)
        return msgpack.unpackb(raw, raw=False, strict_map_key=False)
    return msgpack.unpackb(bd, raw=False, strict_map_key=False)


def encode_bin(obj):
    raw = msgpack.packb(obj, use_single_float=True)
    comp = lz4.block.compress(raw, store_size=False)
    ulen = msgpack.packb(len(raw))
    payload_len = 1 + len(ulen) + len(comp)
    out = bytearray([0xC9])
    out += struct.pack('>I', payload_len)
    out.append(99)
    out += ulen
    out += comp
    return bytes(out)


def set_coord(table, name, coord, scale):
    idx = None
    for i, entry in enumerate(table):
        if entry[0] == name:
            idx = i
            break
    ident = [[1.0, 1.0, 1.0], 1.0]
    if idx is None:
        mods = [ident[:] for _ in range(7)]
        table.append([name, mods])
        idx = len(table) - 1
    mods = table[idx][1]
    while len(mods) <= coord:
        mods.append(ident[:])
    mods[coord] = [[float(x) for x in scale], 1.0]


def main():
    import os
    src = r'C:\Users\pepsi\Downloads\KK_236802 (1).png'
    outdir = 'out/patched'
    os.makedirs(outdir, exist_ok=True)
    prefix, hdr, blocks, marker, version = parse_container(src)
    print('marker', marker, 'version', version, 'blocks', [n for n, _ in blocks])
    variants = {
        'clear6': [('cf_d_sk_top', 6, (1.0, 1.0, 1.0))],
        'source6': [('cf_d_sk_top', 6, (1.0, 1.0, 1.0)),
                    ('cf_d_sk_01_00', 6, (0.3104839324951172, 1.0, 1.0)),
                    ('cf_d_sk_07_00', 6, (0.3104839324951172, 1.0, 1.0))],
        'top_source6': [('cf_d_sk_01_00', 6, (0.3104839324951172, 1.0, 1.0)),
                        ('cf_d_sk_07_00', 6, (0.3104839324951172, 1.0, 1.0))],
    }
    for tag, changes in variants.items():
        bl = []
        for name, block in blocks:
            if name == 'KKEx':
                ext = msgpack.unpackb(block, raw=False, strict_map_key=False)
                ent = ext['KKABMPlugin.ABMData']
                payload = ent[1]
                table = decode_bin(payload['boneData'])
                for bone, coord, scale in changes:
                    set_coord(table, bone, coord, scale)
                payload = dict(payload)
                payload['boneData'] = encode_bin(table)
                ext = dict(ext)
                ext['KKABMPlugin.ABMData'] = [ent[0], payload]
                block = msgpack.packb(ext, use_single_float=True)
            bl.append((name, block))
        rebuild(prefix, hdr, bl, os.path.join(outdir, 'card_%s.png' % tag))


if __name__ == '__main__':
    main()
