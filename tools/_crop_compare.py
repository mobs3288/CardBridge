from PIL import Image
import sys, os
# crop and upscale regions for comparison
pairs = [
 ("C:/Users/pepsi/OneDrive/Pictures/Screenshots/Screenshot 2026-11-24 003906.png", "out/zoom_nonmmd_arms.png", (60,350,740,650)),
 ("C:/Users/pepsi/OneDrive/Pictures/Screenshots/Screenshot 2026-11-24 004107.png", "out/zoom_mmd_arms.png", (40,250,640,560)),
 ("out/repro_1124/Matou Sakura alter_view.png", "out/zoom_exportview_arms.png", (60,150,600,420)),
]
for src,dst,box in pairs:
    im = Image.open(src).convert("RGB")
    c = im.crop(box)
    c = c.resize((c.width*2, c.height*2), Image.LANCZOS)
    c.save(dst)
    print(dst, im.size, box, c.size)
