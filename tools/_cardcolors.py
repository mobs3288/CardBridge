import struct, msgpack, sys, glob, os
def read_card(path):
    b=open(path,'rb').read(); o=8
    while True:
        ln,=struct.unpack('>I',b[o:o+4]); typ=b[o+4:o+8]; o+=12+ln
        if typ==b'IEND': break
    def s(o):
        n=0; sh=0
        while True:
            c=b[o]; o+=1; n|=(c&0x7f)<<sh
            if not c&0x80: break
            sh+=7
        return b[o:o+n].decode('utf-8','replace'), o+n
    o+=4
    marker,o=s(o); ver,o=s(o)
    fl,=struct.unpack('<i',b[o:o+4]); o+=4+fl
    hl,=struct.unpack('<i',b[o:o+4]); o+=4
    hdr=msgpack.unpackb(b[o:o+hl],raw=False,strict_map_key=False); o+=hl+8
    d0=o
    return {i['name']:b[d0+i['pos']:d0+i['pos']+i['size']] for i in hdr['lstInfo']}
def custom(raw):
    o=0; out=[]
    for _ in range(3):
        n,=struct.unpack('<i',raw[o:o+4]); o+=4
        out.append(msgpack.unpackb(raw[o:o+n],raw=False,strict_map_key=False)); o+=n
    return out
for p in sys.argv[1:]:
    try:
        blocks=read_card(p); face,body,hair=custom(blocks['Custom'])
        keys=['skinMainColor','skinSubColor','nipColor','areolaSize','nipId','underhairColor','underhairId']
        print(os.path.basename(p), {k:body.get(k) for k in keys})
    except Exception as e:
        print(os.path.basename(p),'ERR',e)
