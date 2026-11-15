from PIL import Image
import numpy as np
im=Image.open(r"D:\Project K\build-cardbridge\c392340_0\textures\cf_m_body_create_result_5.png").convert("RGBA")
a=np.array(im)
print(im.size)
# list the most saturated pink-ish pixels (r>g>b, r-g>10)
m=(a[:,:,0].astype(int)-a[:,:,1].astype(int)>15)&(a[:,:,1].astype(int)>=a[:,:,2].astype(int)-5)&(a[:,:,3]>200)
print("pink-ish px",m.sum())
if m.sum():
    px=a[m][:,:3]
    print("mean",px.mean(axis=0),"min",px.min(axis=0),"max",px.max(axis=0))
# unique color count
print("unique",len(np.unique(a.reshape(-1,4),axis=0)))
