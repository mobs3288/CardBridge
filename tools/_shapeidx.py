import re

src = open('src/shape/ShapeBodyGenerated.inc', encoding='utf-8').read()


def names(tag):
    m = re.search(r'static const char\* const ' + tag + r'\[\] = \{(.*?)\};', src, re.S)
    return re.findall(r'"([^"]+)"', m.group(1))


dst = names('kBodyDst')
srcn = names('kBodySrc')
print('dst', len(dst), 'src', len(srcn))
for i, n in enumerate(dst):
    if 'sk' in n:
        print('DST', i, n)
print()
for i, n in enumerate(srcn):
    if 'sk' in n:
        print('SRC', i, n)
