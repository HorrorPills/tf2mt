#!/usr/bin/env python3
"""t9dump — inspect a .t9 capture.  t9dump.py <file.t9> [--id N] [--limit N] [--stats]
--id N: every record that mentions object id N (create/get/set/addref/release...), with record numbers."""
import struct, sys, re, collections
names = {}
for m in re.finditer(r'^\s+T9_([A-Z_]+)(?: = (\d+))?,', open(__file__.rsplit('/', 3)[0] + '/tools/trace/t9.h').read().split('enum t9_op')[1], re.M):
    names[len(names) + 1] = m.group(1)
args = sys.argv[1:]
path = args[0]
want = int(args[args.index('--id') + 1]) if '--id' in args else None
limit = int(args[args.index('--limit') + 1]) if '--limit' in args else 10**12
stats = '--stats' in args
# ops whose payload starts with object ids, and how many leading u32 fields are ids
ID_FIELDS = {'SET_TEXTURE': [1], 'SET_STREAM_SOURCE': [1], 'SET_INDICES': [0], 'SET_VDECL': [0], 'SET_VS': [0],
             'SET_PS': [0], 'SET_RT': [1], 'SET_DS': [0], 'STRETCH_RECT': [0, 6], 'UPDATE_SURFACE': [0, 6],
             'UPDATE_TEXTURE': [0, 1], 'GET_RT_DATA': [0, 1], 'COLOR_FILL': [0], 'GET_SURFACE_LEVEL': [0, 1],
             'GET_CUBE_SURFACE': [0, 1], 'GET_RT': [0], 'GET_DS': [0], 'GET_BACKBUFFER': [0], 'ADDREF': [0],
             'RELEASE': [0], 'BUFFER_WRITE': [0], 'SURFACE_WRITE': [0], 'TEXTURE_WRITE': [0], 'VOLUME_WRITE': [0],
             'QUERY_ISSUE': [0], 'QUERY_GETDATA': [0], 'GEN_MIPS': [0], 'LOCK_READ': [0]}
cnt = collections.Counter(); size = collections.Counter()
with open(path, 'rb') as f:
    f.read(16)
    n = 0; frame = 0
    while n < limit:
        h = f.read(8)
        if len(h) < 8: break
        op, sz = struct.unpack('<II', h)
        pl = f.read(sz); n += 1
        name = names.get(op, f'?{op}')
        cnt[name] += 1; size[name] += sz + 8
        if name == 'PRESENT': frame += 1
        if want is None or stats: continue
        k = min(len(pl) // 4, 8)
        u = struct.unpack(f'<{k}I', pl[:k * 4])
        ids = [u[i] for i in ID_FIELDS.get(name, [0] if name.startswith('CREATE_') and name != 'CREATE_DEVICE' else []) if i < k]
        if want in ids:
            print(f'rec {n} frame {frame} {name} {list(u)}')
if stats or want is None:
    for k, v in cnt.most_common():
        print(f'{k:20s} {v:10d} {size[k] / 1e6:10.1f} MB')
