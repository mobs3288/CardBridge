from PIL import Image
import sys, collections

def stats(p):
    im = Image.open(p).convert('RGBA')
    W,H = im.size
    px = im.load()
    hist = collections.Counter()
    for y in range(H):
        for x in range(W):
            a = px[x,y][3]
            hist[a] += 1
    print('FILE', p, 'size', (W,H))
    print('  alpha distinct', len(hist), 'top:', hist.most_common(8))
    # bbox of a>0
    minx,miny,maxx,maxy = W,H,-1,-1
    for y in range(H):
        for x in range(W):
            if px[x,y][3] > 0:
                if x<minx: minx=x
                if x>maxx: maxx=x
                if y<miny: miny=y
                if y>maxy: maxy=y
    print('  alpha>0 bbox', (minx,miny,maxx,maxy), 'of', (W,H))
    # per-row fraction of nonzero alpha, sampled every H/16
    row=[]
    for yy in range(0,H,max(1,H//16)):
        n=0
        for x in range(W):
            if px[x,yy][3]>0: n+=1
        row.append((yy, round(n/W,3)))
    print('  rows(y,frac a>0):', row)
    return im

for p in sys.argv[1:]:
    stats(p)
