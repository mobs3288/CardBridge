"""One-screen summary per card of how CardBridge's export differs from the in-game dump (the sidecar's truth folders).

    python -I -X utf8 audit_cards.py <truth root> <export root> <card> [<card> ...]

<truth root>/<card>/ holds the sidecar dump, <export root>/<card>/ the CardBridge export (the .pmx and textures\\).
Runs compare_truth.py and compare_outfit_colours.py and prints, per card: renderers paired, the game renderers with no CardBridge
mesh, the three largest geometry offsets and the three largest garment texture differences. The body and bra rows are left out of
the geometry list: CardBridge deletes the triangles the clothes hide, the game keeps the whole mesh.
"""
import glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
IGNORED_MISSING = {'o_tang', 'o_shadowcaster'}   # the tongue is hidden by default; the shadow caster is not drawn visibly


def run(script, *args):
    return subprocess.run([sys.executable, '-I', '-X', 'utf8', os.path.join(HERE, script), *args], capture_output=True, text=True).stdout


def main():
    truth_root, export_root = sys.argv[1:3]
    for card in sys.argv[3:]:
        truth, export = os.path.join(truth_root, card), os.path.join(export_root, card)
        pmx = glob.glob(os.path.join(export, '*.pmx'))
        print('=== ' + card)
        if not pmx:
            print('  no export')
            continue
        geo = run('compare_truth.py', truth, pmx[0]).splitlines()
        print('  ' + geo[0])
        rows = [l for l in geo[2:] if re.match(r'\s*[\d.]+cm', l) and 'o_body_a' not in l and 'o_bra_a' not in l]
        for l in rows[:3]:
            print('  geo ' + l.strip())
        for l in geo:
            if l.startswith('missing:'):
                names = [n.split('(')[0] for n in l[len('missing:'):].split(',')]
                names = [n.strip() for n in names if n.strip() not in IGNORED_MISSING]
                if names:
                    print('  missing in CardBridge: ' + ', '.join(names))
        col = run('compare_outfit_colours.py', truth, export).splitlines()
        for l in col[:3]:
            if not l.endswith('compared'):
                print('  col ' + l.strip())
        if col:
            print('  ' + col[-1])


if __name__ == '__main__':
    main()
