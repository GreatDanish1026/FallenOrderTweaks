"""Finds the DXBC shaders in a UE 4.21 GlobalShaderCache-*.bin and the shader type each belongs to.

  python gsc.py <GlobalShaderCache.bin> [name filter] [out dir]

Shader code in the cache is compressed; each blob is located by trying to inflate at every
zlib header, and listed with the shader type names (FStrings) on either side.
"""
import re, struct, sys, zlib, os
d = open(sys.argv[1], 'rb').read()
flt = sys.argv[2] if len(sys.argv) > 2 else ''
out = sys.argv[3] if len(sys.argv) > 3 else None
names = [(m.start(), m.group().decode()) for m in re.finditer(rb'[FT][A-Za-z0-9_]{6,}(PS|VS|CS|GS|HS|DS)[A-Za-z0-9_]*\x00', d)]
names = [(o, n.rstrip('\0')) for o, n in names]
blobs = []
i = 0
while True:
    i = d.find(b'\x78', i)
    if i < 0: break
    if d[i+1] in (0x01, 0x5e, 0x9c, 0xda):
        try:
            z = zlib.decompressobj()
            u = z.decompress(d[i:i+0x400000])
            k = u.find(b'DXBC', 0, 1024)  # after Unreal's own header (packed resource table)
            if k >= 0:
                dxbc = u[k:]
                dxbc = dxbc[:struct.unpack_from('<I', dxbc, 24)[0]]  # Unreal appends its own data
                blobs.append((i, len(d[i:i+0x400000]) - len(z.unused_data), dxbc))
                i += 2; continue
        except zlib.error:
            pass
    i += 1
print(f'{len(names)} names, {len(blobs)} DXBC blobs')
import bisect
offs = [o for o, _ in names]
for o, clen, u in blobs:
    k = bisect.bisect_left(offs, o)
    before = names[k - 1][1] if k > 0 else '?'
    after = names[k][1] if k < len(names) else '?'
    nb = before  # a shader's code follows its type name
    if flt and flt not in before + after: continue
    print(f'{o:#09x} {len(u):6d}  digest {u[4:20].hex()}  before {before}  after {after}')
    if out:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, f'{nb}_{o:x}.dxbc'), 'wb').write(u)
