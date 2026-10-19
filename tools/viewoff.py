# Offset of a uniform buffer member: the `mov dword [rbp-0x20], imm32` (C7 45 E0) just before
# the code loads the member's name.
import struct, sys, re
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
secs = {}
for i in range(nsec):
    o = pe + 24 + optsz + i*40; name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o+8); secs[name] = (va, rsz, raw)
tva, tsz, traw = secs['.text']; text = d[traw:traw+tsz]
rva, rsz, rraw = secs['.rdata']; rdata = d[rraw:rraw+rsz]
for name in sys.argv[2:]:
    pat = b'\0\0' + (name + '\0').encode('utf-16le')
    i = rdata.find(pat)
    while i >= 0 and (i + 2) % 2: i = rdata.find(pat, i + 1)
    target = rva + i + 2
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', text):
        k = m.start()
        if tva + k + 7 + struct.unpack_from('<i', text, k + 3)[0] != target: continue
        w = text[k - 0x80:k]
        j = w.rfind(b'\xc7\x45\xe0')
        off = struct.unpack_from('<I', w, j + 3)[0] if j >= 0 else None
        where = f'cb1[{off // 16}].{"xyzw"[(off % 16) // 4]}' if off is not None else '?'
        print(f'{name:28s} lea {0x140000000 + tva + k:#x}  offset {off if off is None else hex(off)}  {where}')
