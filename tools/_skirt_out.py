import sys, numpy as np
sys.path.insert(0,'tools')
import _pmxprobe as p
from scipy.spatial import cKDTree
d=p.parse('out/kk_custom/Oosuki_Mamako.pmx'); pos=np.asarray(d['pos'],float); mats=d['mats']
def tv(mi):
 idx=mats[mi]['verts']; return pos[idx.reshape(-1,3)]
def clean(t):
 n=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]); l=np.linalg.norm(n,axis=1); return t[l>1e-9],n[l>1e-9]/l[l>1e-9,None]
body,_=clean(tv(0)); sk,n=clean(tv(12)); c=sk.mean(1); cen=np.array([c[:,0].mean(),0,c[:,2].mean()]); r=c-cen; r[:,1]=0; r/=np.linalg.norm(r,axis=1,keepdims=True)+1e-12; n[(n*r).sum(1)<0]*=-1
cent=sk.mean(1); tree=cKDTree(cent)
def cpt(p,a,b,c):
 ab=b-a; ac=c-a; ap=p-a; d1=ab@ap; d2=ac@ap
 if d1<=0 and d2<=0: return a
 bp=p-b; d3=ab@bp; d4=ac@bp
 if d3>=0 and d4<=d3: return b
 vc=d1*d4-d3*d2
 if vc<=0 and d1>=0 and d3<=0: return a+ab*(d1/(d1-d3))
 cp=p-c; d5=ab@cp; d6=ac@cp
 if d6>=0 and d5<=d6: return c
 vb=d5*d2-d1*d6
 if vb<=0 and d2>=0 and d6<=0: return a+ac*(d2/(d2-d6))
 va=d3*d6-d5*d4
 if va<=0 and (d4-d3)>=0 and (d5-d6)>=0: return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)))
 den=1/(va+vb+vc); return a+ab*(vb*den)+ac*(vc*den)
bv=np.unique(body.reshape(-1,3),axis=0); ymin,ymax=sk[:,:,1].min(),sk[:,:,1].max(); band=(bv[:,1]>ymin+0.15)&(bv[:,1]<ymax-0.15); sel=np.where(band)[0]
dist_c,idx_c=tree.query(bv[sel],k=24); res=[]
for kk,vi in enumerate(sel):
 p0=bv[vi]; best=1e9; bp=None; bn=None
 for j in idx_c[kk]:
  cp=cpt(p0,sk[j,0],sk[j,1],sk[j,2]); dd=np.linalg.norm(p0-cp)
  if dd<best: best=dd; bp=cp; bn=n[j]
 sd=(p0-bp)@bn; res.append((vi,sd,best,p0.copy(),bn.copy(),bp.copy()))
out=[x for x in res if x[1]>0.005 and x[2]<0.8]; out.sort(key=lambda x:-x[1])
print('count',len(out))
for x in out[:60]: print('sd %.3f d %.3f pos'% (x[1],x[2]), np.round(x[3],3))
