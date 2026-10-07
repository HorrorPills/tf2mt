#!/usr/bin/env python3
"""Minimal Source RCON client for the local TF2 listen server (launched with -usercon).

Usage: rcon.py [--port 27015] [--password tf2mt] <command> [<command> ...]
Each command is sent separately; responses are printed.
"""
import argparse, socket, struct, sys

AUTH, EXEC, RESPONSE = 3, 2, 0


def packet(rid, kind, body):
    payload = struct.pack('<ii', rid, kind) + body.encode() + b'\0\0'
    return struct.pack('<i', len(payload)) + payload


def recv_packet(s):
    n = struct.unpack('<i', recv_exact(s, 4))[0]
    data = recv_exact(s, n)
    rid, kind = struct.unpack('<ii', data[:8])
    return rid, kind, data[8:-2].decode(errors='replace')


def recv_exact(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('rcon connection closed')
        buf += chunk
    return buf


def run(commands, port=27015, password='tf2mt', timeout=5.0):
    s = socket.create_connection(('127.0.0.1', port), timeout=timeout)
    s.sendall(packet(1, AUTH, password))
    while True:  # server sends an empty RESPONSE then the AUTH_RESPONSE
        rid, kind, _ = recv_packet(s)
        if kind == 2:
            if rid == -1:
                raise PermissionError('rcon auth failed')
            break
    out = []
    for i, cmd in enumerate(commands, start=10):
        s.sendall(packet(i, EXEC, cmd))
        s.sendall(packet(i + 1000, RESPONSE, ''))  # sentinel: marks end of a multi-packet reply
        text = ''
        while True:
            rid, kind, body = recv_packet(s)
            if rid == i + 1000:
                break
            text += body
        out.append(text)
    s.close()
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=27015)
    ap.add_argument('--password', default='tf2mt')
    ap.add_argument('commands', nargs='+')
    a = ap.parse_args()
    for r in run(a.commands, a.port, a.password):
        sys.stdout.write(r)
