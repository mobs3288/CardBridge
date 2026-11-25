import json, sys
p = sys.argv[1]
d = json.load(open(p, encoding='utf-8'))
print('keys:', list(d.keys()))
print('counts:', json.dumps(d.get('counts', {})))
print('textures:', len(d.get('textures', [])))
for t in d.get('textures', []):
    print(json.dumps(t, ensure_ascii=False))
print('\nparts:')
for pt in d.get('parts', []):
    print(json.dumps({k: pt.get(k) for k in ('id','label','group','status','visible','includeInExport','meshes','underPiece','startHidden','mod','bundle') if k in pt}, ensure_ascii=False))
