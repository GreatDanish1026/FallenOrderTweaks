"""Disassembles DXBC files with the system's d3dcompiler_47.dll and lists their chunks.

  python disasm.py <file.dxbc> [...]   -> writes <file>.asm next to each
"""
import ctypes, struct, sys
d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
class Blob(ctypes.c_void_p): pass
for path in sys.argv[1:]:
    code = open(path, 'rb').read()
    n = struct.unpack_from('<I', code, 28)[0]
    chunks = [code[o:o+4].decode() for o in struct.unpack_from(f'<{n}I', code, 32)]
    out = ctypes.c_void_p()
    hr = d3dc.D3DDisassemble(code, len(code), 0, None, ctypes.byref(out))
    if hr != 0: print(path, 'D3DDisassemble failed', hex(hr & 0xffffffff)); continue
    vt = ctypes.cast(ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    ptr = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(out)
    size = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(out)
    text = ctypes.string_at(ptr, size).decode('latin-1').rstrip('\0')
    open(path.rsplit('.', 1)[0] + '.asm', 'w').write(text)
    lines = text.count('\n')
    print(f'{path}: chunks {chunks}, {lines} lines')
