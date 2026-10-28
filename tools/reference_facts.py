"""CardBridge Milestone 1a - Python reference.

Produces the golden facts the C++ implementation must reproduce, using UnityPy
(MIT) and msgpack, entirely from files: no game process. Research tool only; it
is not shipped and is not a runtime dependency of CardBridge.

    python -I -X utf8 reference_facts.py <card.png> <mods-folder> <abdata-folder> [<zipmod>] > golden.json

The output holds counts, names and hashes only - no game or mod assets.
"""
import json, os, struct, sys, zipfile

import msgpack
import UnityPy


def fnv1a64(b: bytes) -> int:
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def mp(b):
    return msgpack.unpackb(b, raw=False, strict_map_key=False)


def read_card(path):
    b = open(path, 'rb').read()
    o = 8
    while True:
        ln, = struct.unpack('>I', b[o:o + 4]); typ = b[o + 4:o + 8]; o += 12 + ln
        if typ == b'IEND':
            break

    def s(o):
        n = 0; sh = 0
        while True:
            c = b[o]; o += 1; n |= (c & 0x7f) << sh
            if not c & 0x80: break
            sh += 7
        return b[o:o + n].decode(), o + n
    o += 4
    marker, o = s(o); ver, o = s(o)
    fl, = struct.unpack('<i', b[o:o + 4]); o += 4 + fl
    hl, = struct.unpack('<i', b[o:o + 4]); o += 4
    hdr = mp(b[o:o + hl]); o += hl + 8
    data0 = o
    blocks = {i['name']: b[data0 + i['pos']:data0 + i['pos'] + i['size']] for i in hdr['lstInfo']}
    return marker, ver, hdr['lstInfo'], blocks


def split_custom(raw):
    o = 0; out = []
    for _ in range(3):
        n, = struct.unpack('<i', raw[o:o + 4]); o += 4
        out.append(mp(raw[o:o + n])); o += n
    return out


def split_coord(blob):
    q = 0; parts = []
    for _ in range(2):
        n, = struct.unpack('<i', blob[q:q + 4]); q += 4
        parts.append(mp(blob[q:q + n])); q += n
    return parts


def mat_name(m):
    try:
        return m.read().m_Name
    except Exception:
        return '(external)'   # the material lives in another bundle


def prefab_facts(env, prefab):
    objs = {o.path_id: o for o in env.objects}
    gos = {p: o.read() for p, o in objs.items() if o.type.name == 'GameObject'}
    trs = {p: o.read() for p, o in objs.items() if o.type.name == 'Transform'}
    go2tr = {t.m_GameObject.path_id: p for p, t in trs.items()}
    roots = [p for p, g in gos.items() if g.m_Name == prefab]
    sub = set()
    stack = [go2tr[roots[0]]]
    while stack:
        t = trs[stack.pop()]
        sub.add(t.m_GameObject.path_id)
        stack += [c.path_id for c in t.m_Children]
    rends = []
    bones = set()
    for o in env.objects:
        if o.type.name != 'SkinnedMeshRenderer':
            continue
        s = o.read()
        if s.m_GameObject.path_id not in sub:
            continue
        mesh = s.m_Mesh.read()
        tris = sum(sm.indexCount // 3 for sm in mesh.m_SubMeshes)
        names = []
        for b in s.m_Bones:
            try: names.append(gos[b.read().m_GameObject.path_id].m_Name)
            except Exception: pass
        bones.update(names)
        rends.append({'renderer': gos[s.m_GameObject.path_id].m_Name, 'enabled': bool(s.m_Enabled), 'mesh': mesh.m_Name,
                      'vertices': mesh.m_VertexData.m_VertexCount, 'triangles': tris, 'bones': len(names),
                      'materials': [mat_name(m) for m in s.m_Materials]})
    tex = {}
    for o in env.objects:
        if o.type.name == 'Texture2D':
            t = o.read()
            rgba = t.image.convert('RGBA').tobytes()
            tex[t.m_Name] = {'width': t.m_Width, 'height': t.m_Height, 'format': int(t.m_TextureFormat), 'rgba_fnv1a64': str(fnv1a64(rgba))}
    return {'renderers': rends, 'bone_names': sorted(bones), 'textures': tex}


def main(card, mods, abdata, zipmod=None):
    marker, ver, lst, blocks = read_card(card)
    face, body, hair = split_custom(blocks['Custom'])
    outfits = [split_coord(x) for x in mp(blocks['Coordinate'])]
    kkex = mp(blocks['KKEx'])
    rows = [mp(x) for x in kkex['com.bepis.sideloader.universalautoresolver'][1]['info']]
    facts = {'marker': marker, 'format_version': ver, 'blocks': [b['name'] for b in lst], 'outfits': len(outfits),
             'resolver_rows': len(rows), 'mods': sorted({r['ModID'] for r in rows}),
             'outfit0_clothes_ids': [p['id'] for p in outfits[0][0]['parts']],
             'outfit0_accessory_ids': [(p['type'], p['id']) for p in outfits[0][1]['parts'] if p['type'] != 120]}

    # resolve the top (category 105) through its zipmod
    top = [r for r in rows if r['Property'] == 'outfit0.ChaFileClothes.ClothesTop'][0]
    zpath = zipmod
    for dp, dn, fn in (os.walk(mods) if not zpath else []):
        for f in fn:
            if f.lower().endswith(('.zipmod', '.zip')):
                p = os.path.join(dp, f)
                try:
                    if f'<guid>{top["ModID"]}</guid>' in zipfile.ZipFile(p).read('manifest.xml').decode('utf-8-sig'):
                        zpath = p; break
                except Exception:
                    pass
        if zpath: break
    z = zipfile.ZipFile(zpath)
    entry = None
    for n in z.namelist():
        if n.lower().startswith('abdata/list/characustom/') and n.lower().endswith('.csv'):
            lines = z.read(n).decode('utf-8-sig').splitlines()
            if lines[0].split(',')[0] == str(top['CategoryNo']):
                hdr = lines[3].split(',')
                for ln in lines[4:]:
                    row = dict(zip(hdr, ln.split(',')))
                    if row.get('ID') == str(top['Slot']): entry = row
    facts['top_resolution'] = {'mod_file': os.path.basename(zpath), 'main_ab': entry['MainAB'], 'main_data': entry['MainData']}
    env = UnityPy.load(z.read('abdata/' + entry['MainAB']))
    facts['top_prefab'] = prefab_facts(env, entry['MainData'])

    base = UnityPy.load(os.path.join(abdata, 'chara', 'oo_base.unity3d'))
    bf = prefab_facts(base, 'p_cf_body_00')
    facts['body'] = [r for r in bf['renderers'] if r['renderer'] == 'o_body_a']
    json.dump(facts, sys.stdout, indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main(*sys.argv[1:5])
