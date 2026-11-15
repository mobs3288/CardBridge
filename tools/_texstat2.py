from PIL import Image
import numpy as np, glob, os
for p in sorted(glob.glob(r"D:\Project K\build-cardbridge\c392340_0\textures\*body*.png")):
    im=Image.open(p).convert("RGBA"); a=np.array(im)
    m=(a[:,:,0].astype(int)-a[:,:,1].astype(int)>15)&(a[:,:,3]>200)
    print(os.path.basename(p), im.size, "pink", int(m.sum()), "mean", (a[m][:,:3].mean(axis=0) if m.sum() else None), "unique", len(np.unique(a.reshape(-1,4),axis=0)))
