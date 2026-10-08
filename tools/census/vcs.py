#!/usr/bin/env python3
"""Extract every shader combo from Valve's .vcs containers (version 6) inside hl2_misc_dir.vpk.

Usage: vcs.py <TF2DIR> <corpus_out_dir> [manifest.json.gz]
Writes <fnv1a64>.{vs,ps}.bin (same naming/hash as tools/trace census capture) and a manifest
{vcs file: {"static": n, "combos": [[static_id, dynamic_id, hash], ...]}} for pre-warming.

VCS v6 layout (as observed): header 7×u32 {version=6, total_combos, dynamic_combos, flags, centroid_mask,
n_static_records, source_crc}; n_static_records × {static_id, file_offset}; u32 n_aliases + aliases × {id, source}.
At each file_offset: blocks, each a u32 header (0xffffffff = end; bit 30 = LZMA-packed "LZMA" + u32 unpacked +
u32 packed + 5 property bytes; bit 31 = stored) whose payload is a run of {u32 dynamic_id, u32 size, bytecode}.
"""
import gzip, json, lzma, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from vpk import VPK

import ctypes
_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), '../../build/tools/libfnv.dylib'))   # tools/census/fnv.c
_lib.fnv1a64.restype = ctypes.c_uint64
_lib.fnv1a64.argtypes = [ctypes.c_char_p, ctypes.c_size_t]

def fnv1a64(b):
    return _lib.fnv1a64(bytes(b), len(b))

def blocks(d, off):
    while off + 4 <= len(d):                      # the last static combo may run to end-of-file
        hdr = struct.unpack_from('<I', d, off)[0]
        if hdr == 0xffffffff:
            return
        size = hdr & 0x3fffffff
        payload = d[off + 4:off + 4 + size]
        if hdr & 0x40000000:
            magic, unpacked, packed = struct.unpack_from('<4sII', payload)
            assert magic == b'LZMA', magic
            filt = lzma._decode_filter_properties(lzma.FILTER_LZMA1, payload[12:17])
            yield lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[filt]).decompress(payload[17:17 + packed], max_length=unpacked)
        else:
            yield payload
        off += 4 + size

def combos(block):
    q = 0
    while q + 8 <= len(block):
        cid, sz = struct.unpack_from('<II', block, q)
        yield cid, block[q + 8:q + 8 + sz]
        q += 8 + sz

def main():
    tf2dir, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    vpk = VPK(os.path.join(tf2dir, 'hl2/hl2_misc_dir.vpk'))
    names = sorted(n for n in vpk.entries if n.startswith('shaders/fxc/') and n.endswith('.vcs'))
    manifest, unique, total = {}, set(), 0
    for name in names:
        d = vpk.read(name)
        ver = struct.unpack_from('<I', d)[0]
        if ver == 2:   # old uncompressed container: 5×u32 header (version, total, dynamic, flags, centroid), then {size, code} per combo
            tot = struct.unpack_from('<I', d, 4)[0]
            off, entries = 20, []
            for cid in range(tot):
                if off + 4 > len(d):
                    break
                sz = struct.unpack_from('<I', d, off)[0]
                code = d[off + 4:off + 4 + sz]
                off += 4 + sz
                kind = 'ps' if code[2:4] == b'\xff\xff' else 'vs' if code[2:4] == b'\xfe\xff' else None
                if kind:
                    h = fnv1a64(code)
                    if h not in unique:
                        unique.add(h)
                        with open(os.path.join(out, f'{h:016x}.{kind}.bin'), 'wb') as f:
                            f.write(code)
                    entries.append([0, cid, f'{h:016x}']); total += 1
            manifest[os.path.basename(name)] = {'static': 1, 'combos': entries}
            print(f'{os.path.basename(name):55s} (v2) combos {len(entries)}'); continue
        ver, tot, dyn, flags, cmask, nstat, crc = struct.unpack_from('<7I', d)
        if ver != 6:
            print(f'skip {name}: version {ver} (not used by the DX9 path)'); continue
        recs = struct.unpack_from(f'<{2 * nstat}I', d, 28)
        entries = []
        for i in range(nstat):
            sid, off = recs[2 * i], recs[2 * i + 1]
            if sid == 0xffffffff or off >= len(d):        # sentinel record marks the end of the data
                continue
            for blk in blocks(d, off):
                for did, code in combos(blk):
                    tok = struct.unpack_from('<I', code)[0] if len(code) >= 4 else 0
                    kind = 'ps' if tok >> 16 == 0xffff else 'vs' if tok >> 16 == 0xfffe else None
                    if not kind:
                        continue
                    h = fnv1a64(code)
                    if h not in unique:
                        unique.add(h)
                        p = os.path.join(out, f'{h:016x}.{kind}.bin')
                        if not os.path.exists(p):
                            with open(p, 'wb') as f:
                                f.write(code)
                    entries.append([sid, did, f'{h:016x}'])
                    total += 1
        manifest[os.path.basename(name)] = {'static': nstat, 'combos': entries}
        print(f'{os.path.basename(name):55s} static {nstat:6d}  combos {len(entries):7d}  unique so far {len(unique)}', flush=True)
    if len(sys.argv) > 3:
        with gzip.open(sys.argv[3], 'wt') as f:
            json.dump(manifest, f)
    print(f'{len(names)} vcs files, {total} combos, {len(unique)} unique shaders')

if __name__ == '__main__':
    main()
