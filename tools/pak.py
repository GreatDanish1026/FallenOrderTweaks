"""Minimal reader for unencrypted UE 4.21 pak files (version 7): list and extract.

  python pak.py <file.pak> list "<glob>"
  python pak.py <file.pak> extract "<glob>" <out dir>

Handles stored, zlib and Oodle entries. Oodle needs the game's oo2core DLL: point the
OODLE_DLL environment variable at it (Fallen Order ships one under
Engine\\Binaries\\ThirdParty\\Oodle).
"""
import ctypes
import fnmatch
import os
import struct
import sys
import zlib

PAK_MAGIC = 0x5A6F12E1
OODLE = os.environ.get('OODLE_DLL', 'oo2core_5_win64.dll')
# FPakEntry::CompressionMethod in version 7: flags of ECompressionFlags.
COMPRESS_NONE, COMPRESS_ZLIB, COMPRESS_GZIP, COMPRESS_CUSTOM = 0, 1, 2, 4


class R:
    def __init__(s, b, o=0): s.b, s.o = b, o
    def u8(s): v = s.b[s.o]; s.o += 1; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.o)[0]; s.o += 4; return v
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; return v
    def u64(s): v = struct.unpack_from('<Q', s.b, s.o)[0]; s.o += 8; return v
    def raw(s, n): v = s.b[s.o:s.o + n]; s.o += n; return v

    def fstr(s):  # FString: length with terminator; negative = UTF-16
        n = s.i32()
        if n == 0: return ''
        if n < 0: return s.raw(-n * 2).decode('utf-16le')[:-1]
        return s.raw(n)[:-1].decode('latin-1')


def read_entry(r):
    """FPakEntry as serialized (version 7): in the index and again in front of the data."""
    e = dict(offset=r.u64(), size=r.u64(), usize=r.u64(), method=r.u32())
    r.raw(20)  # SHA-1
    e['blocks'] = [(r.u64(), r.u64()) for _ in range(r.u32())] if e['method'] else []
    e['enc'] = r.u8()
    e['bs'] = r.u32()
    return e


class Pak:
    def __init__(s, path):
        s.f = open(path, 'rb')
        s.f.seek(os.path.getsize(path) - 256)
        t = s.f.read()
        i = t.rfind(struct.pack('<I', PAK_MAGIC))
        if i < 17: raise RuntimeError('not a pak file')
        if t[i - 1]: raise RuntimeError('encrypted index')
        _, s.version, ioff, isz = struct.unpack_from('<IiQQ', t, i)
        if s.version != 7: print(f'warning: pak version {s.version}, written for 7', file=sys.stderr)
        s.f.seek(ioff)
        r = R(s.f.read(isz))
        s.mount = r.fstr()
        s.files = {}
        for _ in range(r.i32()):
            name = r.fstr()
            s.files[name] = read_entry(r)

    def read(s, name):
        e = s.files[name]
        if e['enc']: raise RuntimeError('encrypted entry')
        s.f.seek(e['offset'])
        header = R(s.f.read(8 + 8 + 8 + 4 + 20 + 4 + 16 * len(e['blocks']) + 1 + 4))
        read_entry(header)
        if e['method'] == COMPRESS_NONE:
            return s.f.read(e['usize'])
        out = bytearray()
        left = e['usize']
        for start, end in e['blocks']:  # relative to the entry (version 5+)
            s.f.seek(e['offset'] + start)
            src = s.f.read(end - start)
            want = min(e['bs'], left)
            if e['method'] & COMPRESS_ZLIB:
                block = zlib.decompress(src)
            elif e['method'] & COMPRESS_CUSTOM:
                fn = ctypes.WinDLL(OODLE).OodleLZ_Decompress
                fn.restype = ctypes.c_int64
                dst = ctypes.create_string_buffer(want)
                got = fn(src, len(src), dst, want, 1, 0, 0, None, 0, None, None, None, 0, 3)
                if got != want: raise RuntimeError(f'oodle returned {got}, expected {want} ({name})')
                block = dst.raw
            else:
                raise RuntimeError(f"unsupported compression {e['method']}")
            out += block
            left -= len(block)
        return bytes(out)


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[2] not in ('list', 'extract') or (sys.argv[2] == 'extract' and len(sys.argv) < 5):
        sys.exit(__doc__)
    p = Pak(sys.argv[1])
    pattern = sys.argv[3].lower()
    for n in p.files:
        if not fnmatch.fnmatch(n.lower(), pattern): continue
        if sys.argv[2] == 'list':
            e = p.files[n]
            print(f"{n}  {e['usize']} bytes, method {e['method']}")
        else:
            dst = os.path.join(sys.argv[4], n)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(p.read(n))
            print('extracted', n)
