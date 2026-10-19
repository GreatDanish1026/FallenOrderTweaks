# Votes on IConsoleManager::Singleton and the vtable offsets the game's code uses right after
# `lea rdx, [rip+L"<cvar>"]`.
import struct, sys, collections, re
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = {}
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode()
    vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    secs[name] = (va, vsz, raw, rsz)
print('sections', {k: hex(v[0]) for k, v in secs.items()}, 'imagebase', hex(base))
tva, tvsz, traw, trsz = secs['.text']; text = d[traw:traw+trsz]
rva_, rvsz, rraw, rrsz = secs['.rdata']; rdata = d[rraw:rraw+rrsz]
def wide_rva(s):
    pat = (s + '\0').encode('utf-16le')
    i = 0
    while True:
        i = rdata.find(pat, i)
        if i < 0: return None
        if i % 2 == 0 and i >= 2 and rdata[i-2:i] == b'\0\0': return rva_ + i
        i += 1
names = sys.argv[2:]
single = collections.Counter(); find = collections.Counter(); after = collections.Counter(); hits = 0
leare = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
targets = {}
for n in names:
    r = wide_rva(n)
    if r: targets[r] = n
print('found strings', len(targets), 'of', len(names))
for m in leare.finditer(text):
    i = m.start()
    disp = struct.unpack_from('<i', text, i + 3)[0]
    tgt = tva + i + 7 + disp
    if tgt not in targets: continue
    if (text[i+2] & 0x38) >> 3 != 2 or text[i] != 0x48: continue  # want rdx
    hits += 1
    for q in range(i - 7, i - 0x40, -1):
        if text[q:q+3] == b'\x48\x8b\x0d':
            single[tva + q + 7 + struct.unpack_from('<i', text, q + 3)[0]] += 1; break
    w = text[i+7:i+0x30]
    for k in range(len(w) - 2):
        if w[k] == 0xff and w[k+1] in (0x50, 0x90):
            off = w[k+2] if w[k+1] == 0x50 else struct.unpack_from('<i', w, k + 2)[0]
            find[off] += 1
            # next virtual call through the returned object
            w2 = w[k+3:]
            for j in range(len(w2) - 2):
                if w2[j] == 0xff and w2[j+1] in (0x50, 0x90):
                    off2 = w2[j+2] if w2[j+1] == 0x50 else struct.unpack_from('<i', w2, j + 2)[0]
                    after[off2] += 1; break
            break
print('hits', hits)
print('singleton rva votes', [(hex(a), c) for a, c in single.most_common(4)])
print('FindConsoleVariable offset votes', [(hex(a), c) for a, c in find.most_common(5)])
print('following call offsets', [(hex(a), c) for a, c in after.most_common(8)])
