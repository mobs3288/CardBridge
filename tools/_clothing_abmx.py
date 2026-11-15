import struct
import sys

import msgpack

sys.path.insert(0, 'tools')
import _abmx as A


def dotnet_string(b, o):
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


def read_clothing(path):
    b = open(path, 'rb').read()
    o = 8
    while True:
        ln, = struct.unpack('>I', b[o:o + 4])
        typ = b[o + 4:o + 8]
        o += 12 + ln
        if typ == b'IEND':
            break
    o += 4
    marker, o = dotnet_string(b, o)
    ver, o = dotnet_string(b, o)
    name, o = dotnet_string(b, o)
    n, = struct.unpack('<i', b[o:o + 4])
    o += 4 + n
    print('marker', marker, 'ver', ver, 'name', name, 'coordbytes', n, 'tail', len(b) - o)
    ext = None
    if o < len(b):
        try:
            extName, p = dotnet_string(b, o)
            print('ext name', repr(extName), 'at', o, 'after', p)
            if extName == 'KKEx':
                # version+size or size only; try both
                a, = struct.unpack('<i', b[p:p + 4])
                b2, = struct.unpack('<i', b[p + 4:p + 8])
                print('  ints', a, b2, 'next', b[p + 8:p + 12].hex(), 'remaining', len(b) - p - 8)
                candidates = []
                if 0 <= b2 <= len(b) - p - 8:
                    candidates.append((p + 8, b2))
                if 0 <= a <= len(b) - p - 4:
                    candidates.append((p + 4, a))
                for off, size in candidates:
                    try:
                        val = msgpack.unpackb(b[off:off + size], raw=False, strict_map_key=False)
                        if isinstance(val, dict):
                            ext = val
                            print('  parsed ext at', off, 'size', size, 'keys', list(val)[:8])
                            break
                    except Exception as e:
                        print('  candidate', off, size, 'failed', e)
        except Exception as e:
            print('ext parse err', e)
    return marker, ext


if __name__ == '__main__':
    for p in sys.argv[1:] or [r'C:\Users\pepsi\Downloads\KKCLOTHING_012669.png']:
        marker, ext = read_clothing(p)
        print('==', p)
        if ext:
            print(' keys:', list(ext))
            # reuse _abmx dict decoding
            ent = ext.get('KKABMPlugin.ABMData')
            if ent:
                print(' ABMX entry version', ent[0])
                bd = ent[1].get('boneData')
                print(' boneData bytes', len(bd) if bd else None)
                if bd:
                    import io
                    if bd[0] in (0xc7, 0xc8, 0xc9):
                        lb = {0xc7: 1, 0xc8: 2, 0xc9: 4}[bd[0]]
                        assert bd[1 + lb] == 99
                        o = 1 + lb + 1
                        un = msgpack.Unpacker(io.BytesIO(bd[o:]), raw=False, strict_map_key=False)
                        ulen = un.unpack()
                        o += un.tell()
                        raw = A.lz4_decompress_block(bd[o:], ulen)
                        lst = msgpack.unpackb(raw, raw=False, strict_map_key=False)
                    else:
                        lst = msgpack.unpackb(bd, raw=False, strict_map_key=False)
                    print(' ABMX bones:', len(lst))
                    for name, mods in lst:
                        print('  ', name, 'n=', len(mods))
                        for i, m in enumerate(mods):
                            print('      [%d] scale=%s len=%s pos=%s rot=%s' % (i, m[0] if len(m) > 0 else None,
                                                                                 m[1] if len(m) > 1 else None,
                                                                                 m[2] if len(m) > 2 else None,
                                                                                 m[3] if len(m) > 3 else None))
