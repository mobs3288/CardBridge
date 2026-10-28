import UnityPy, struct, os, ctypes, sys, re, lz4.block

"""Extracts the compiled Shader Forge shaders of Koikatsu from an AssetBundle and disassembles them (research tool).

The game ships its shaders inside the asset bundles that use them, as Direct3D 11 bytecode (Unity platform 4). This
tool decompresses a shader's program blob (LZ4), carves the DXBC containers out of it and disassembles each one with
the Windows d3dcompiler (Windows only). Output goes to a folder that is git-ignored (game assets).

    python -I -X utf8 extract_shaders.py <out dir> <bundle> [shader name ...]

Needs: pip install UnityPy lz4. Seen so far: co_top_00 (main_opaque, main_alpha, main_emblem, main_opaque_low),
bo_head_00 (main_skin, toon_eye_lod0, toon_eyew_lod0, toon_nose_lod0, toon_glasses_lod0, main_item),
bo_hair_b_00 / bo_hair_f_00 (main_hair, main_hair_front, *_low), ao_head_00 (main_color), oo_base (mnpb, shadowcast).
"""
OUT = os.path.abspath(sys.argv[1])
os.makedirs(OUT, exist_ok=True)

d3d = ctypes.WinDLL('d3dcompiler_47.dll')
d3d.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
d3d.D3DDisassemble.restype = ctypes.c_long

class Blob(ctypes.Structure):
    pass

def disasm(data):
    out = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(data, len(data))
    hr = d3d.D3DDisassemble(buf, len(data), 0, None, ctypes.byref(out))
    if hr != 0:
        return None
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vt = ctypes.cast(ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    p, n = getptr(out), getsize(out)
    return ctypes.string_at(p, n).decode('latin-1')

def carve(blob):
    """All DXBC containers in a decompressed Unity subprogram blob."""
    res = []
    i = 0
    while True:
        i = blob.find(b'DXBC', i)
        if i < 0: break
        size, = struct.unpack_from('<I', blob, i + 24)
        res.append((i, blob[i:i + size]))
        i += max(size, 4)
    return res

def run(bundle, wanted):
    env = UnityPy.load(bundle)
    seen = set()
    for o in env.objects:
        if o.type.name != 'Shader': continue
        s = o.read()
        name = s.m_ParsedForm.m_Name
        if name in seen or (wanted and name not in wanted): continue
        seen.add(name)
        comp, dec = s.compressedLengths[0], s.decompressedLengths[0]
        raw = bytes(s.compressedBlob)
        blob = lz4.block.decompress(raw, uncompressed_size=dec)
        progs = carve(blob)
        safe = name.replace('/', '_').replace(' ', '')
        open(os.path.join(OUT, safe + '.blob'), 'wb').write(blob)
        kinds = {}
        for k, (off, bc) in enumerate(progs):
            asm = disasm(bc)
            stage = 'vs' if asm and re.search(r'^\s*vs_', asm, re.M) else 'ps' if asm and re.search(r'^\s*ps_', asm, re.M) else '??'
            kinds[stage] = kinds.get(stage, 0) + 1
            open(os.path.join(OUT, '%s_%03d_%s.asm' % (safe, k, stage)), 'w', encoding='utf-8').write(asm or '(disassembly failed)')
        print(name, 'blob', len(blob), 'programs', len(progs), kinds)

if __name__ == '__main__':
    run(sys.argv[2], set(sys.argv[3:]))
