from PIL import Image
import numpy as np, glob, os
for p in glob.glob(r"D:\Project K\cardbridge\out\skintest\textures\*.png"):
    im=Image.open(p).convert("RGBA"); a=np.array(im)
    print(os.path.basename(p), im.size, "unique", len(np.unique(a.reshape(-1,4),axis=0)), "alphaMin", int(a[:,:,3].min()))
