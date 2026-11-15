p = r'D:\Project K\cardbridge\src\assemble\Assembler.cpp'
b = open(p, 'rb').read()
print('BOM' if b[:3] == b'\xef\xbb\xbf' else 'no BOM')
t = b.decode('utf-8')
t = t.lstrip('﻿')  # strip a stray BOM char if present after decode
bad = '\ufffd\u0090”'
good = '—'
n = t.count(bad)
t = t.replace(bad, good)
t = t.replace('â€”', good).replace('â', 'a')
open(p, 'wb').write(t.encode('utf-8'))
print('fixed bad-seq count', n)
