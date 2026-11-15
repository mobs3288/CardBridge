import subprocess
orig = subprocess.run(['git', 'show', 'HEAD:src/assemble/Assembler.cpp'], capture_output=True).stdout.decode('utf-8')
cur = open(r'D:\Project K\cardbridge\src\assemble\Assembler.cpp', encoding='utf-8').read()
probe = ['is fully transparent', 'achromatic (a white/grey fill', 'Clochette mt_nipple03 bump', 'f.property +']
for p in probe:
    io_ = orig.find(p)
    ic_ = cur.find(p)
    same = io_ >= 0 and ic_ >= 0 and orig[io_:io_ + 60] == cur[ic_:ic_ + 60]
    print(p[:40], '| orig', io_, '| cur', ic_, '|', 'SAME' if same else 'DIFFERENT')
