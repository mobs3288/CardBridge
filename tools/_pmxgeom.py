import struct, sys

def read_str(b, pos, enc):
    n = struct.unpack_from('<i', b, pos)[0]; pos += 4
    s = b[pos:pos+n].decode('utf-16-le' if enc == 0 else 'utf-8', 'replace'); pos += n
    return s, pos

def idx(b,pos,size):
    v={1:b[pos],2:struct.unpack_from('<H',b,pos)[0],4:struct.unpack_from('<I',b,pos)[0]}[size]
    return v,pos+size

p = sys.argv[1]
b = open(p,'rb').read()
assert b[:4]==b'PMX '
gcount = b[8]
enc,add,vis,tis,mis,bis,morphis,rbis = struct.unpack_from('<8B',b,9)
pos=9+gcount
for _ in range(4): _,pos=read_str(b,pos,enc)
nv, = struct.unpack_from('<i',b,pos); pos+=4
vpos=[]
for i in range(nv):
    x,y,z = struct.unpack_from('<3f',b,pos); pos+=12
    pos+=12  # normal
    pos+=8   # uv
    pos+=16*add  # add vec4
    wt=b[pos]; pos+=1
    if wt==0: _,pos=idx(b,pos,bis)
    elif wt==1: _,pos=idx(b,pos,bis); _,pos=idx(b,pos,bis); pos+=4
    elif wt in (2,4):
        for _ in range(4): _,pos=idx(b,pos,bis)
        pos+=16
    elif wt==3:
        _,pos=idx(b,pos,bis); _,pos=idx(b,pos,bis); pos+=4
        pos+=36  # SDEF C/R0/R1
    pos+=4  # edge scale
    vpos.append((x,y,z))
ni, = struct.unpack_from('<i',b,pos); pos+=4   # CardBridge writes the INDEX count
faces=[]
for i in range(0,ni,3):
    a,pos=idx(b,pos,vis); c,pos=idx(b,pos,vis); d,pos=idx(b,pos,vis)
    faces.append((a,c,d))
nu, = struct.unpack_from('<i',b,pos); pos+=4
for i in range(nu):
    _,pos=read_str(b,pos,enc)
nm, = struct.unpack_from('<i',b,pos); pos+=4
mats=[]
for i in range(nm):
    n,pos=read_str(b,pos,enc); ne,pos=read_str(b,pos,enc)
    diff=struct.unpack_from('<4f',b,pos); pos+=16
    spec=struct.unpack_from('<3f',b,pos); pos+=12
    spow,=struct.unpack_from('<f',b,pos); pos+=4
    amb=struct.unpack_from('<3f',b,pos); pos+=12
    df,=struct.unpack_from('<B',b,pos); pos+=1
    ec=struct.unpack_from('<4f',b,pos); pos+=16
    es,=struct.unpack_from('<f',b,pos); pos+=4
    ti,pos=idx(b,pos,tis); si,pos=idx(b,pos,tis)
    sf,=struct.unpack_from('<B',b,pos); pos+=1
    tflag,=struct.unpack_from('<B',b,pos); pos+=1
    if tflag==0: toon,pos=idx(b,pos,tis)
    else: pos+=1
    memo,pos=read_str(b,pos,enc)
    cnt,=struct.unpack_from('<i',b,pos); pos+=4
    mats.append((n,diff[3],cnt,memo))

for name,alpha,cnt,memo in mats:
    if 'sock' in name.lower():
        print('MAT', name, 'alpha', alpha, 'memo', memo)
fstart=0
for name,alpha,cnt,memo in mats:
    if ('sock' in name.lower() or 'under' in name.lower() or 'body/' in name.lower()) and cnt>0:
        ys=[]; xs=[]; zs=[]
        for f in faces[fstart:fstart+cnt//3]:
            for vi in f:
                x,y,z=vpos[vi]; xs.append(x); ys.append(y); zs.append(z)
        if ys:
            print(f'{name:40s} alpha={alpha:.2f} tris={cnt//3} y=[{min(ys):.2f},{max(ys):.2f}] x=[{min(xs):.2f},{max(xs):.2f}] memo={memo[:70]}')
    fstart+=cnt//3
