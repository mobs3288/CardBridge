import sys

sys.path.insert(0, 'tools')
import _abmx as A


def mod(m):
    scale = m[0] if len(m) > 0 else [1, 1, 1]
    length = m[1] if len(m) > 1 else 1.0
    pos = m[2] if len(m) > 2 else [0, 0, 0]
    rot = m[3] if len(m) > 3 else [0, 0, 0]
    return scale, length, pos, rot


def fmt(m):
    s, l, p, r = mod(m)
    bits = []
    if any(abs(x - 1) > 1e-6 for x in s):
        bits.append('scl=(%.3f,%.3f,%.3f)' % tuple(s))
    if abs(l - 1) > 1e-6:
        bits.append('len=%.3f' % l)
    if any(abs(x) > 1e-6 for x in p):
        bits.append('pos=(%.3f,%.3f,%.3f)' % tuple(p))
    if any(abs(x) > 1e-6 for x in r):
        bits.append('rot=(%.3f,%.3f,%.3f)' % tuple(r))
    return ' '.join(bits) or 'identity'


def nonident(mods):
    return [i for i, m in enumerate(mods) if fmt(m) != 'identity']


for path in sys.argv[1:]:
    marker, ab = A.load_abmx(path)
    print('==', path, '  marker', marker, 'bones', len(ab))
    for name in sorted(ab):
        mods = ab[name]
        ni = nonident(mods)
        if not ni:
            continue
        short = ' '.join('%d:%s' % (i, fmt(mods[i])) for i in ni)
        print('  %-22s n=%d  %s' % (name, len(mods), short))
