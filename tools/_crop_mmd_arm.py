from PIL import Image
import numpy as np
im = Image.open("C:/Users/pepsi/OneDrive/Pictures/Screenshots/Screenshot 2026-11-24 004107.png").convert("RGB")
# Crop a tight region around the character's left arm (screen right) and right arm
for name, box in [("out/mmd_rightarm.png",(430,260,620,400)),("out/mmd_leftarm.png",(40,260,230,400)),("out/mmd_torso.png",(250,180,430,420))]:
    c = im.crop(box); c = c.resize((c.width*4,c.height*4), Image.LANCZOS); c.save(name); print(name,c.size)
# Print pixel colours along the arm line (screen right) y=330, x 440..600
a=np.array(im)
for y in (300,320,340,360):
    row=[]
    for x in range(440,620,10):
        row.append(f"{x}:{tuple(a[y,x])}")
    print("y",y," ".join(row))
