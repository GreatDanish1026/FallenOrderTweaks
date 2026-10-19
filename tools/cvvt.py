import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i*40; name = d[o:o+8].rstrip(b'\0').decode(); vsz, sva, rsz, raw = struct.unpack_from('<IIII', d, o+8); secs.append((name, sva, max(vsz, rsz), raw))
def off(v):
    r = v - 0x140000000
    for n, sva, sz, raw in secs:
        if sva <= r < sva + sz: return raw + r - sva
def q(v): return struct.unpack_from('<Q', d, off(v))[0]
def in_sec(v, name): return any(n == name and sva <= v - 0x140000000 < sva + sz for n, sva, sz, raw in secs)
def classify(f):
    if off(f) is None: return "??"
    b = d[off(f):off(f)+12]
    if b[:3] == b'\xb0\x01\xc3': return 'TRUE'
    if b[:3] == b'\x32\xc0\xc3': return 'false'
    if b[:3] == b'\x33\xc0\xc3': return 'null'
    if b[:3] == b'\x48\x8d\x41' and b[4] == 0xc3: return f'&this+{b[3]:#x}'
    if b[:3] == b'\x48\x8b\xc1' and b[3] == 0xc3: return 'this'
    if b[:3] == b'\x8b\x41' and False: pass
    return b[:8].hex()
MGR = 0x1429d8c60
seen = set()
for slot in range(0x18, 0x48, 8):
    fn = q(MGR + slot); code = d[off(fn):off(fn)+0x200]
    for k in range(len(code) - 7):
        if code[k] in (0x48, 0x4c) and code[k+1] == 0x8d and (code[k+2] & 0xc7) == 0x05:
            tgt = fn + k + 7 + struct.unpack_from('<i', code, k+3)[0]
            if not in_sec(tgt, '.rdata') or tgt in seen: continue
            if not in_sec(q(tgt), '.text') or not in_sec(q(tgt+8), '.text'): continue
            seen.add(tgt)
            row = [classify(q(tgt + i*8)) for i in range(0, 20)]
            print(f'mgr slot {slot:#x} -> vtable {tgt:#x}')
            for i, c in enumerate(row): print(f'    {i*8:#04x} {c}')
            break
