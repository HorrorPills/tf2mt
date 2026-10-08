#!/usr/bin/env python3
"""check_parity.py <probe tf2mt.tsv> <census_root> <caps_variant>: compare tf2mt's IDirect3D9 answers (probe output)
against what DXVK told TF2 in the census: identity, D3DCAPS9 bytes, every format/depth/type query, display modes."""
import glob, os, struct, sys
probe, root, variant = sys.argv[1:4]
rows = [l.rstrip('\n').split('\t') for l in open(probe)]
kv = {r[0]: r[1:] for r in rows if len(r) > 1}
ident = open(glob.glob(f'{root}/{variant}/adapter-*.bin')[0], 'rb').read()
caps = open(glob.glob(f'{root}/{variant}/caps-*.bin')[0], 'rb').read()
cs = lambda b: b.split(b'\0')[0].decode('latin1')
vendor, device, subsys, rev = struct.unpack_from('<4I', ident, 1064)
expect = {'identity.driver': cs(ident[:512]), 'identity.devicename': cs(ident[1024:1056]),
          'identity.driverversion': hex(struct.unpack_from('<q', ident, 1056)[0]), 'identity.vendor': hex(vendor),
          'identity.device': hex(device), 'identity.subsys': hex(subsys), 'identity.revision': str(rev),
          'identity.whql': str(struct.unpack_from('<I', ident, 1096)[0]), 'identity.guid': ident[1080:1096].hex()}
bad = []
for k, v in expect.items():
    if kv.get(k, ['?'])[0] != v: bad.append(f'{k}: tf2mt {kv.get(k)} != census {v}')
got = b''.join(bytes.fromhex(kv[f'caps.{i:03x}'][0]) for i in range(0, 304, 16))
if got != caps: bad.append('caps bytes differ')
q = [r for r in rows if r[0] == 'query']
qbad = [r for r in q if r[-2] != r[-1]]       # reference answer (last field of the original line) vs ours
bad += [f'query {" ".join(r[1:])}' for r in qbad]
census_modes = set()
for f in glob.glob(f'{root}/*/census-*.tsv'):
    if f.endswith('.threads.tsv'): continue
    for line in open(f, errors='replace'):
        if line.startswith('mode|fmt=22 '): census_modes.add(line.split('|')[1].split()[1])
ours = {r[3] for r in rows if r[0] == 'mode' and r[1] == '22'}
mode_note = f'modes: tf2mt {len(ours)}, census {len(census_modes)}, only-census {sorted(census_modes - ours)[:5]}, only-tf2mt {sorted(ours - census_modes)[:5]}'
print(f'identity+caps checked; {len(q)} census queries compared, {len(qbad)} mismatches; {mode_note}')
print('displaymode:', kv.get('displaymode'))
print('PARITY OK' if not bad else 'MISMATCHES:\n  ' + '\n  '.join(bad[:25]))
sys.exit(1 if bad else 0)
