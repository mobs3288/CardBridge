from PIL import Image
import numpy as np
p=r"D:\Project K\cardbridge\out\skintest_bake\textures\baked_cf_m_body_117.png"
im=Image.open(p).convert("RGBA"); a=np.array(im).astype(int)
r,g,b,al=a[:,:,0],a[:,:,1],a[:,:,2],a[:,:,3]
sat=(r-g)+(g-b)
mask=(al>200)
ys,xs=np.where(mask)
satm=sat.copy(); satm[~mask]=-999
idx=np.dstack(np.unravel_index(np.argsort(satm.ravel())[-30:], satm.shape))[0]
for y,x in idx[::-1]:
    print(x,y,a[y,x])
print('unique colors top by count:')
cols,counts=np.unique(a[a[:,:,3]>200].reshape(-1,4),axis=0,return_counts=True)
order=np.argsort(counts)[-15:][::-1]
for i in order: print(cols[i],counts[i])
