import sys, numpy as np
sys.path.insert(0,'tools')
import _pmxprobe as p
from scipy.spatial import cKDTree
d=p.parse('out/kk_custom/Oosuki_Mamako.pmx'); pos=np.asarray(d['pos'],float); mats=d['mats']
def tv(mi):
    idx=mats[mi]['verts']; return pos[idx.reshape(-1,3)]
def clean(t):
    n=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]); l=np.linalg.norm(n,axis=1); return t[l>1e-9], (n[l>1e-9]/l[l>1e-9,None])
body,_=clean(tv(0)); sk,n=clean(tv(12))
# orient normals outward radially
c=sk.mean(1); cen=np.array([c[:,0].mean(),0,c[:,2].mean()]); r=c-cen; r[:,1]=0; r/=np.linalg.norm(r,axis=1,keepdims=True)+1e-12
dl=(n*r).sum(1); flip=dl<0; n[flip]*=-1
cent=sk.mean(1); tree=cKDTree(cent)
def closest_point_triangle(p,a,b,c):
    ab=b-a; ac=c-a; ap=p-a
    d1=ab@ap; d2=ac@ap
    if d1<=0 and d2<=0: return a
    bp=p-b; d3=ab@bp; d4=ac@bp
    if d3>=0 and d4<=d3: return b
    vc=d1*d4-d3*d2
    if vc<=0 and d1>=0 and d3<=0:
        v=d1/(d1-d3); return a+ab*v
    cp=p-c; d5=ab@cp; d6=ac@cp
    if d6>=0 and d5<=d6: return c
    vb=d5*d2-d1*d6
    if vb<=0 and d2>=0 and d6<=0:
        w=d2/(d2-d6); return a+ac*w
    va=d3*d6-d5*d4
    if va<=0 and (d4-d3)>=0 and (d5-d6)>=0:
        w=(d4-d3)/((d4-d3)+(d5-d6)); return b+(c-b)*w
    den=1.0/(va+vb+vc); v=vb*den; w=vc*den
    return a+ab*v+ac*w
bv=np.unique(body.reshape(-1,3),axis=0)
ymin,ymax=sk[:,:,1].min(),sk[:,:,1].max(); band=(bv[:,1]>ymin+0.15)&(bv[:,1]<ymax-0.15)
sel=np.where(band)[0]
print('band body verts',len(sel))
# only within 1.5 units of a skirt center
dist_c,idx_c=tree.query(bv[sel],k=24)
res=[]
for kk,vi in enumerate(sel):
    p0=bv[vi]; best=1e9; bp=None; bn=None
    for j in idx_c[kk]:
        cp=closest_point_triangle(p0,sk[j,0],sk[j,1],sk[j,2]); dd=np.linalg.norm(p0-cp)
        if dd<best: best=dd; bp=cp; bn=n[j]
    sd=(p0-bp)@bn
    res.append((vi,sd,best,p0.copy(),bn.copy(),bp.copy()))
res.sort(key=lambda x:-x[1])
print('top positive signed distances (outside through skirt):')
for x in res[:20]: print('vi',x[0],'sd %.4f dist %.4f pos'% (x[1],x[2]),np.round(x[3],3),'n',np.round(x[4],2))
print('counts sd>0.01',sum(1 for x in res if x[1]>0.01),'sd<-0.01',sum(1 for x in res if x[1]<-0.01),'near',sum(1 for x in res if x[2]<0.5))
print('positive near 0.5',sum(1 for x in res if x[1]>0.01 and x[2]<0.5))
print('sd percentiles among near<0.5',np.percentile([x[1] for x in res if x[2]<0.5],[0,1,5,10,25,50,75,90,95,99,100]))
