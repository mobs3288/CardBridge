import glob
import os
import sys

sys.path.insert(0, 'tools')
import _cardcolors as c

labels = ['Height', 'Head size', 'Neck width', 'Neck thick', 'Bust size', 'Bust vert', 'Bust angle LR',
          'Bust spacing', 'Bust angle UD', 'Bust depth', 'Bust round', 'Areola puff', 'Nipple thick',
          'Nipple depth', 'Shoulder-chest width', 'Shoulder-chest depth', 'Chest width', 'Chest depth',
          'Ribs width', 'Ribs depth', 'Torso height', 'Belly', 'Waist width', 'Waist depth', 'Hip width',
          'Hip depth', 'Butt size', 'Butt angle', 'Thigh width', 'Thigh depth', 'Lower thigh width',
          'Lower thigh depth', 'Calf width', 'Calf depth', 'Shin', 'Ankle width', 'Ankle depth',
          'Shoulder width', 'Shoulder thick', 'Upper arm width', 'Upper arm thick', 'Forearm width',
          'Forearm thick', 'Wrist']

rows = []
for p in glob.glob(r'C:\Users\pepsi\Downloads\KK_*.png'):
    try:
        b = c.read_card(p)
        face, body, hair = c.custom(b['Custom'])
        vals = body['shapeValueBody']
        tb = body.get('typeBone', 0)
        rows.append((p, vals, tb))
    except Exception:
        pass

rows.sort(key=lambda r: r[1][26] if len(r[1]) > 26 else 0, reverse=True)
print('%-22s %4s %4s %4s %4s %4s %4s %4s %s' % ('card', 'bust', 'hipW', 'hipD', 'butt', 'bAng', 'thW', 'thD', 'typeBone'))
for p, v, tb in rows:
    print('%-22s %4.2f %4.2f %4.2f %4.2f %4.2f %4.2f %4.2f %d' % (
        os.path.basename(p), v[4], v[24], v[25], v[26], v[27], v[28], v[29], tb))
