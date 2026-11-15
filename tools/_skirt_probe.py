import sys, numpy as np
sys.path.insert(0,'tools')
import _pmxprobe as p
from scipy.spatial import cKDTree

d=p.parse('out/kk_custom/Oosuki_Mamako.pmx')
pos=np.asarray(d['pos'],dtype=np.float64)
mats=d['mats']

def tri_verts(mi):
    idx=mats[mi]['verts']
    return pos[idx.reshape(-1,3)]

body=tri_verts(0)
skirt=tri_verts(12)
# remove degenerate
def clean(t):
    n=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]); l=np.linalg.norm(n,axis=1)
    return t[l>1e-9]
body=clean(body); skirt=clean(skirt)
print('body tris',len(body),'skirt tris',len(skirt))
# normals of skirt
sn=np.cross(skirt[:,1]-skirt[:,0],skirt[:,2]-skirt[:,0]); sn/=np.linalg.norm(sn,axis=1,keepdims=True)
# body vertex set
bv=np.unique(body.reshape(-1,3),axis=0)
print('body verts uniq',len(bv))
# nearest skirt vertex per body vertex
tree=cKDTree(skirt.reshape(-1,3))
dist,ii=tree.query(bv)
print('body->skirt dist percentiles',np.percentile(dist,[0,1,5,10,25,50,75,90,95,99,100]))
# restrict y range of skirt
ymin,ymax=skirt[:,:,1].min(),skirt[:,:,1].max()
print('skirt y',ymin,ymax)
sel=(bv[:,1]>=ymin)&(bv[:,1]<=ymax)
print('body verts in skirt y',sel.sum())
print('dist in band percentiles',np.percentile(dist[sel],[0,1,5,10,25,50,75,90,95,99,100]))
# radial: body verts inside xz radius of skirt?
print('nearest skirt vert idx sample',ii[sel][:10])
# count body verts closer than 0.1 (pmx units) to any skirt vertex
for th in [0.02,0.05,0.1,0.2,0.3,0.5]:
    print('under',th, (dist[sel]<th).sum())
