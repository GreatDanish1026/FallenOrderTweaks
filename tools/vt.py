import struct, sys
d = open(sys.argv[1], 'rb').read(); va = int(sys.argv[2], 16); n = int(sys.argv[3])
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
def off(v):
    r = v - 0x140000000
    for i in range(nsec):
        o = pe + 24 + optsz + i*40; vsz, sva, rsz, raw = struct.unpack_from('<IIII', d, o+8)
        if sva <= r < sva + max(vsz, rsz): return raw + r - sva
for i in range(n):
    print(hex(i*8), hex(struct.unpack_from('<Q', d, off(va) + i*8)[0]))
