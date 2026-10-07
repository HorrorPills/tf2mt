#!/usr/bin/env python3
"""Binary patch for the runtime's winemac.so (Sikarugir Wine 10.0, x86_64): stop discarding mouse motion
on cursor warps.

Wine's -[WineApplicationController setCursorPosition:] (dlls/winemac.drv/cocoa_app.m) discards all queued
mouse-move events and ignores every event timestamped before the warp. TF2 recentres the cursor every frame,
so ~2/3 of mouse motion was dropped: camera updated ~40x/s at 120 fps (measured, docs/mouse-input.md).
Patch (verified against original bytes first):
  0x8399  movsd %xmm0,0x128(%r15)   (lastSetCursorPositionTime = uptime)  -> 9-byte NOP
  0x848b  movl  $0x1800,%edx        (discard mask MOUSE_MOVED_*)          -> movl $0,%edx
Usage: winemac_warp_nodiscard.py apply|revert|status
"""
import hashlib, os, shutil, subprocess, sys

HOME_DIR = os.environ.get('TF2_HOME', os.path.expanduser('~/Games/tf2'))
SO = HOME_DIR + '/wine/lib/wine/x86_64-unix/winemac.so'
BACKUP = HOME_DIR + '/backups/winemac.so.orig'
PATCHES = [(0x8399, 'f2410f118728010000', '0f1f80000000006690'),
           (0x848b, 'ba00180000', 'ba00000000')]

def state(b):
    if all(b[o:o + len(bytes.fromhex(a))] == bytes.fromhex(a) for o, a, _ in PATCHES): return 'original'
    if all(b[o:o + len(bytes.fromhex(n))] == bytes.fromhex(n) for o, _, n in PATCHES): return 'patched'
    return 'unknown'

def install(data):
    tmp = SO + '.new'
    open(tmp, 'wb').write(data); os.chmod(tmp, 0o755)
    subprocess.run(['codesign', '-f', '-s', '-', tmp], check=True, capture_output=True)
    os.replace(tmp, SO)   # atomic: running processes keep the old inode

cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
b = bytearray(open(SO, 'rb').read()); st = state(b)
if cmd == 'status':
    print(st, hashlib.sha256(b).hexdigest()[:16])
elif cmd == 'apply':
    if st == 'patched': print('already patched'); sys.exit(0)
    if st != 'original': sys.exit('unexpected winemac.so contents (different Wine build?) — not patching')
    if not os.path.exists(BACKUP): os.makedirs(os.path.dirname(BACKUP), exist_ok=True); shutil.copy2(SO, BACKUP)
    for o, _, n in PATCHES: b[o:o + len(bytes.fromhex(n))] = bytes.fromhex(n)
    install(bytes(b)); print('patched (backup:', BACKUP + ')')
elif cmd == 'revert':
    if not os.path.exists(BACKUP): sys.exit('no backup at ' + BACKUP)
    install(open(BACKUP, 'rb').read()); print('reverted to original')
