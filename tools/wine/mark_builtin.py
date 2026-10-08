#!/usr/bin/env python3
"""Stamp a PE as a Wine builtin DLL: Wine recognises builtins by the signature "Wine builtin DLL" at file
offset 0x40 (inside the DOS stub, before the PE header). Only then does it load the matching unix library."""
import sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
sig = b'Wine builtin DLL\0'
pe = int.from_bytes(b[0x3c:0x40], 'little')
assert pe >= 0x40 + len(sig), f'PE header at {pe:#x} overlaps the signature area'
b[0x40:0x40 + len(sig)] = sig
open(p, 'wb').write(b)
print(f'{p}: marked as Wine builtin')
