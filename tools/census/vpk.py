#!/usr/bin/env python3
"""Minimal Valve VPK (v1/v2) reader: list entries and read file data from *_dir.vpk + numbered archives."""
import os, struct

class VPK:
    def __init__(self, dir_path):
        self.dir_path = dir_path
        self.prefix = dir_path[:-len('_dir.vpk')]
        self.entries = {}
        with open(dir_path, 'rb') as f:
            sig, ver, tree = struct.unpack('<III', f.read(12))
            assert sig == 0x55aa1234, 'not a VPK'
            hdr = 12 + (16 if ver == 2 else 0)
            f.seek(hdr)
            self.data_start = hdr + tree       # archive index 0x7fff = data stored after the tree in _dir.vpk
            def s():
                b = bytearray()
                while (c := f.read(1)) != b'\0':
                    b += c
                return b.decode(errors='replace')
            while (ext := s()):
                while (d := s()):
                    while (n := s()):
                        crc, pre, idx, off, ln, term = struct.unpack('<IHHIIH', f.read(18))
                        preload = f.read(pre)
                        name = (n if d == ' ' else f'{d}/{n}') + ('' if ext == ' ' else f'.{ext}')
                        self.entries[name] = (idx, off, ln, preload)

    def read(self, name):
        idx, off, ln, pre = self.entries[name]
        if ln == 0:
            return pre
        path = self.dir_path if idx == 0x7fff else f'{self.prefix}_{idx:03d}.vpk'
        with open(path, 'rb') as f:
            f.seek(off + (self.data_start if idx == 0x7fff else 0))
            return pre + f.read(ln)
