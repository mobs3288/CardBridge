"""Compares the colours CardBridge gives each material with the ones the real game ended up with (sidecar dump).

    CARDBRIDGE_DEBUG_MAT=1 cardbridge convert <card> ... 2> mats.txt      (the lines that start with "mat ")
    python -I -X utf8 compare_materials.py <truth folder for the card> <mats.txt>

Lists every _Color/_Color2/_Color3 that differs by more than 0.02 in any channel. Eyebrow and eyeline colours are baked into their
textures by CardBridge (the material stays white), so those lines are expected.
"""
import json, re, sys

truth = json.load(open(sys.argv[1] + '/renderers.json', encoding='utf-8'))
ours = {}
for l in open(sys.argv[2], encoding='utf-8'):
    m = re.match(r'mat (.+?) shader=(.*?) idx=-?\d+ tex:(.*?) \| props:(.*)', l.strip())
    if not m: continue
    cols = {}
    for k in re.finditer(r'(_\w+)=\(([-\d. ]+)\)', m.group(4)):
        cols.setdefault(k.group(1), []).append([float(x) for x in k.group(2).split()])
    ours.setdefault(m.group(1), []).append(cols)
seen = set()
for r in truth:
    if not r.get('active') or not r.get('enabled'): continue
    for mt in r['materials']:
        if not mt: continue
        n = mt['name'].split(' (Instance)')[0]
        if n not in ours: continue
        for k in ('_Color', '_Color2', '_Color3'):
            g = mt['colors'].get(k)
            cand = [o.get(k) for o in ours[n] if o.get(k)]
            if g is None or not cand: continue
            d = min(max(abs(a - b) for a, b in zip(g[:3], o[0][:3])) for o in cand)
            if d > 0.02 and (n, k) not in seen:
                seen.add((n, k))
                print('%-34s %-8s game %s ours %s' % (n, k, [round(x, 2) for x in g[:3]], [round(x, 2) for x in cand[0][0][:3]]))
