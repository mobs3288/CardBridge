import struct, sys
from PIL import Image
import numpy as np

def read_str(b, pos, enc):
    n = struct.unpack_from('<i', b, pos)[0]; pos += 4
    return b[pos:pos+n].decode('utf-16-le' if enc==0 else 'utf-8','replace'), pos+n

def idx(b,pos,size):
    v={1:b[pos],2:struct.unpack_from('<H',b,pos)[0],4:struct.unpack_from('<I',b,pos)[0]}[size]
    return v,pos+size

pmx, texpath, want = sys.argv[1], sys.argv[2], sys.argv[3].lower()
b=open(pmx,'rb').read(); g=b[8]; enc,add,vis,tis,mis,bis,morphis,rbis=struct.unpack_from('<8B',b,9); pos=9+g
for _ in range(4): _,pos=read_str(b,pos,enc)
nv,=struct.unpack_from('<i',b,pos); pos+=4
uvs=[]; poss=[]; nrms=[]
for i in range(nv):
    p=struct.unpack_from('<3f',b,pos); pos+=12
    n=struct.unpack_from('<3f',b,pos); pos+=12
    u,v=struct.unpack_from('<2f',b,pos); pos+=8
    uvs.append((u,v)); poss.append(p); nrms.append(n)
    pos+=16*add
    wt=b[pos]; pos+=1
    if wt==0: _,pos=idx(b,pos,bis)
    elif wt==1: _,pos=idx(b,pos,bis); _,pos=idx(b,pos,bis); pos+=4
    elif wt in (2,4):
        for _ in range(4): _,pos=idx(b,pos,bis)
        pos+=16
    elif wt==3:
        _,pos=idx(b,pos,bis); _,pos=idx(b,pos,bis); pos+=44
    pos+=4
ni,=struct.unpack_from('<i',b,pos); pos+=4
faces=[]
for i in range(0,ni,3):
    a,pos=idx(b,pos,vis); c,pos=idx(b,pos,vis); d,pos=idx(b,pos,vis)
    faces.append((a,c,d))
nu,=struct.unpack_from('<i',b,pos); pos+=4
for i in range(nu): _,pos=read_str(b,pos,enc)
nm,=struct.unpack_from('<i',b,pos); pos+=4
mats=[]
for i in range(nm):
    n,pos=read_str(b,pos,enc); ne,pos=read_str(b,pos,enc)
    pos+=16+12+4+12
    df=struct.unpack_from('<B',b,pos)[0]; pos+=1
    pos+=16+4
    ti,pos=idx(b,pos,tis); si,pos=idx(b,pos,tis)
    pos+=1
    tf,=struct.unpack_from('<B',b,pos); pos+=1
    if tf==0: _,pos=idx(b,pos,tis)
    else: pos+=1
    memo,pos=read_str(b,pos,enc)
    cnt,=struct.unpack_from('<i',b,pos); pos+=4
    mats.append((n,cnt,memo,df))
a=np.array(Image.open(texpath).convert('RGBA')); H,W=a.shape[:2]; al=a[:,:,3]
fstart=0
for name,cnt,memo,df in mats:
    if want in name.lower():
        print('MAT',name,'tris',cnt//3,'drawFlags',hex(df),'memo',memo)
        lo=[]; hi=[]
        for f in faces[fstart:fstart+cnt//3]:
            us=[uvs[v][0] for v in f]; vs=[uvs[v][1] for v in f]
            u=sum(us)/3; v=sum(vs)/3
            u-=np.floor(u); v-=np.floor(v)
            x=min(W-1,int(u*W)); y=min(H-1,int((1-v)*H))
            alpha=al[y,x]
            aa,bb,cc=[np.array(poss[v]) for v in f]
            g1=bb-aa; g2=cc-aa; gn=np.cross(g1,g2)
            vn=np.mean([np.array(nrms[v]) for v in f],axis=0)
            facing=float(np.dot(gn,vn))
            (lo if alpha<64 else hi).append(facing)
        print('low-alpha tris',len(lo),'front-facing',sum(1 for x in lo if x>0),'back',sum(1 for x in lo if x<0))
        print('hi-alpha tris ',len(hi),'front-facing',sum(1 for x in hi if x>0),'back',sum(1 for x in hi if x<0))
    fstart+=cnt//3
