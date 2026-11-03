"""Copies the game's colour-grading table (AmplifyColor's "Lut_Amount", a 1024x32 strip = 32 slices of 32x32) into a CardBridge library.

    python -I -X utf8 extract_colour_grade.py <game folder> <library folder>

The character maker and the game run every frame through this table; the viewer applies it too so colours match (ColourGrade.cpp). It is a
game asset: the output goes to <library>/colorgrade/lut_amount.png, which is git-ignored with the rest of the library. Needs UnityPy.
"""
import glob, os, sys
import UnityPy

game, library = sys.argv[1:3]
for bundle in sorted(glob.glob(os.path.join(glob.escape(game), 'abdata', 'map', 'scene', '*.unity3d'))):
    for o in UnityPy.load(bundle).objects:
        if o.type.name != 'Texture2D':
            continue
        t = o.read()
        if getattr(t, 'm_Name', '') == 'Lut_Amount':
            out = os.path.join(library, 'colorgrade')
            os.makedirs(out, exist_ok=True)
            t.image.convert('RGB').save(os.path.join(out, 'lut_amount.png'))
            print('wrote', os.path.join(out, 'lut_amount.png'), 'from', os.path.basename(bundle))
            sys.exit(0)
sys.exit('Lut_Amount not found in the game folder')
