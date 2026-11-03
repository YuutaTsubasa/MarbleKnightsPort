import struct, sys, capstone
path, needle = sys.argv[1], sys.argv[2].encode()
d = open(path, 'rb').read()
shoff, = struct.unpack_from('<Q', d, 0x28); shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
secs = []
for i in range(shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', d, shoff + i * shentsize)
    secs.append([name, typ, flags, addr, off, size])
strtab = secs[shstrndx]
for s in secs:
    s[0] = d[strtab[4] + s[0]: d.index(b'\0', strtab[4] + s[0])].decode()
def off2va(o):
    for s in secs:
        if s[4] <= o < s[4] + s[5] and s[3]: return s[3] + o - s[4]
targets = []
i = d.find(needle)
while i >= 0:
    # walk back to string start
    j = i
    while d[j-1] != 0: j -= 1
    targets.append(off2va(j)); i = d.find(needle, i + 1)
print('string VAs', [hex(t) for t in targets if t])
text = next(s for s in secs if s[0] == '.text')
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
code = d[text[4]:text[4] + text[5]]
import struct as st
words = st.unpack(f'<{len(code)//4}I', code)
pages = {}
for k, w in enumerate(words):
    pc = text[3] + 4 * k
    if (w & 0x9F000000) == 0x90000000:
        imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
        if imm & (1 << 20): imm -= 1 << 21
        pages[w & 31] = ((pc >> 12) + imm) << 12
    elif (w & 0xFF800000) == 0x91000000:
        rn = (w >> 5) & 31
        if rn in pages:
            t = pages[rn] + ((w >> 10) & 0xFFF)
            if t in targets: print('xref at', hex(pc))
