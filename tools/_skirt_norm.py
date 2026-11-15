import sys, numpy as np
sys.path.insert(0,'tools')
import _pmxprobe as p
d=p.parse('out/kk_custom/Oosuki_Mamako.pmx'); pos=np.asarray(d['pos'],float); mats=d['mats']
def tv(mi):
    idx=mats[mi]['verts']; return pos[idx.reshape(-1,3)]
sk=tv(12)
n=np.cross(sk[:,1]-sk[:,0],sk[:,2]-sk[:,0]); l=np.linalg.norm(n,axis=1); ok=l>1e-9; sk=sk[ok]; n=n[ok]/l[ok,None]
c=sk.mean(1); axis=np.array([c[:,0].mean(),0,c[:,2].mean()])
r=c-axis; r[:,1]=0; r/=np.linalg.norm(r,axis=1,keepdims=True)+1e-12
dl=(n*r).sum(1)
print('normal radial dot percentiles',np.percentile(dl,[0,1,5,10,25,50,75,90,95,99,100]))
print('outward triangles', (dl>0).mean())
# Area weighted
print('area weighted outward', np.where(dl>0,l*0.5,0).sum()/ (l*0.5).sum())
