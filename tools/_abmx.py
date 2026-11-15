import struct
import sys

import msgpack


def lz4_decompress_block(src, ulen):
    try:
        import lz4.block
        return lz4.block.decompress(src, uncompressed_size=ulen)
    except ImportError:
        pass
    # minimal LZ4 block decoder (enough for ABMX payloads)
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                x = src[i]
                i += 1
                lit += x
                if x != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                x = src[i]
                i += 1
                ml += x
                if x != 255:
                    break
        ml += 4
        start = len(out) - off
        for k in range(ml):
            out.append(out[start + k])
    return bytes(out[:ulen])


def read_blocks(path):
    b = open(path, 'rb').read()
    o = 8
    while True:
        ln, = struct.unpack('>I', b[o:o + 4])
        typ = b[o + 4:o + 8]
        o += 12 + ln
        if typ == b'IEND':
            break

    def s(o):
        n = 0
        sh = 0
        while True:
            ch = b[o]
            o += 1
            n |= (ch & 0x7f) << sh
            if not ch & 0x80:
                break
            sh += 7
        return b[o:o + n].decode('utf-8', 'replace'), o + n

    o += 4
    marker, o = s(o)
    ver, o = s(o)
    fl, = struct.unpack('<i', b[o:o + 4])
    o += 4 + fl
    hl, = struct.unpack('<i', b[o:o + 4])
    o += 4
    hdr = msgpack.unpackb(b[o:o + hl], raw=False, strict_map_key=False)
    o += hl + 8
    d0 = o
    return marker, {i['name']: b[d0 + i['pos']:d0 + i['pos'] + i['size']] for i in hdr['lstInfo']}


def load_abmx(path):
    marker, blocks = read_blocks(path)
    ext = blocks.get('KKEx')
    if not ext:
        return marker, {}
    ext = msgpack.unpackb(ext, raw=False, strict_map_key=False)
    ent = ext.get('KKABMPlugin.ABMData')
    if not ent or len(ent) < 2:
        return marker, {}
    bd = ent[1].get('boneData')
    if not bd:
        return marker, {}
    if bd[0] in (0xc7, 0xc8, 0xc9):
        lenbytes = {0xc7: 1, 0xc8: 2, 0xc9: 4}[bd[0]]
        assert bd[1 + lenbytes] == 99, bd[:8]
        o = 1 + lenbytes + 1
        import io
        un = msgpack.Unpacker(io.BytesIO(bd[o:]), raw=False, strict_map_key=False)
        ulen = un.unpack()
        o += un.tell()
        raw = lz4_decompress_block(bd[o:], ulen)
        lst = msgpack.unpackb(raw, raw=False, strict_map_key=False)
    else:
        lst = msgpack.unpackb(bd, raw=False, strict_map_key=False)
    out = {}
    if isinstance(lst, dict):
        # ABMX v2: {bone name: one [scale, length, position, rotation] mod}
        for name, m in lst.items():
            out[name] = [m]
        return marker, out
    for entry in lst:
        # v3: [bone, List<BoneModifierData>] or [bone, List<BoneModifierData>, BoneLocation]
        out[entry[0]] = entry[1]
    return marker, out


def show(path, only=None):
    marker, ab = load_abmx(path)
    print('==', path, 'marker', marker, 'bones', len(ab))
    for name in sorted(ab):
        if only and not any(k in name for k in only):
            continue
        mods = ab[name]
        rows = []
        for i, m in enumerate(mods):
            scale = m[0] if len(m) > 0 else None
            length = m[1] if len(m) > 1 else 1.0
            pos = m[2] if len(m) > 2 else None
            rot = m[3] if len(m) > 3 else None
            ident = (scale is None or tuple(round(x, 6) for x in scale) == (1, 1, 1)) and abs(length - 1) < 1e-6 and \
                    (pos is None or all(abs(x) < 1e-9 for x in pos)) and \
                    (rot is None or all(abs(x) < 1e-9 for x in rot))
            rows.append(('.' if ident else 'M') + ('' if len(m) <= 3 else 'r'))
        print('  %-24s n=%d %s' % (name, len(mods), ' '.join(rows)))
        if any(len(m) > 0 and m[0] != [1, 1, 1] for m in mods):
            for i, m in enumerate(mods):
                print('       [%d] scale=%s len=%s pos=%s rot=%s' % (i, m[0] if len(m) > 0 else None,
                                                                     m[1] if len(m) > 1 else None,
                                                                     m[2] if len(m) > 2 else None,
                                                                     m[3] if len(m) > 3 else None))


if __name__ == '__main__':
    keys = ('sk', 'waist', 'siri', 'thigh', 'hip', 'koshi', 'kokan')
    args = sys.argv[1:]
    if not args:
        args = [r'C:\Users\pepsi\Downloads\KK_236802 (1).png', r'C:\Users\pepsi\Downloads\KKCLOTHING_012669.png']
    for p in args:
        show(p, keys)
