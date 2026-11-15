from PIL import Image
import numpy as np, glob, os
for p in sorted(glob.glob(r"D:\Project K\cardbridge\out\skintest_bake\textures\*body*.png")):
    im=Image.open(p).convert("RGBA"); a=np.array(im)
    print(os.path.basename(p), im.size, "alphaMin", int(a[:,:,3].min()), "unique", len(np.unique(a.reshape(-1,4),axis=0)))
    m=(a[:,:,0].astype(int)-a[:,:,1].astype(int)>20)&(a[:,:,3]>200)
    print("  pink px", int(m.sum()))
    if m.sum(): print("  pink mean", a[m][:,:3].mean(axis=0))
