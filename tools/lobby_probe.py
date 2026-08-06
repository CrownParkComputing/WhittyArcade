#!/usr/bin/env python3
"""Diagnose MANX multiplayer lobby discovery (UDP 35109) across two machines.

Speaks the same wire format as src/multiplayer_lobby.cpp, so it can be run
instead of, or alongside, MANX on either machine.

  Machine A:  python3 tools/lobby_probe.py
  Machine B:  python3 tools/lobby_probe.py

Each prints every lobby packet it receives, and which address it came from.
If MANX is already running on the other machine, run this on one machine only
and you should see MANX's hellos arriving.
"""

import argparse
import socket
import struct
import sys
import time

PORT = 35109
MAGIC = b'WAL1'
LOG_MAGIC = b'WAL2'
# magic[4] version node reserved nonce games launch_sequence game[32] + pad
FMT = '<4sBBHQQI32s4x'
SIZE = struct.calcsize(FMT)
assert SIZE == 64, SIZE
# magic[4] version reserved bytes nonce first_line line_count text[768]
LOG_FMT = '<4sBBHQII768s'
LOG_SIZE = struct.calcsize(LOG_FMT)
assert LOG_SIZE == 792, LOG_SIZE


def broadcast_targets():
    """Every plausible broadcast destination, per interface where possible."""
    targets = {'255.255.255.255'}
    try:
        import ipaddress
        import subprocess
        out = subprocess.run(['ip', '-o', '-4', 'addr', 'show'],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if 'inet' not in parts:
                continue
            cidr = parts[parts.index('inet') + 1]
            net = ipaddress.ip_network(cidr, strict=False)
            if net.prefixlen < 31 and not net.is_loopback:
                targets.add(str(net.broadcast_address))
    except Exception as exc:  # noqa: BLE001 - best effort only
        print(f'(could not enumerate interfaces: {exc})', file=sys.stderr)
    return sorted(targets)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--listen-only', action='store_true',
                        help='do not transmit, just show what arrives')
    parser.add_argument('--to', action='append', default=[],
                        help='extra unicast/broadcast target (repeatable)')
    parser.add_argument('--follow', action='store_true',
                        help="print the peer's relayed console output")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(('', PORT))
        owns = True
    except OSError:
        sock.bind(('', 0))
        owns = False
    sock.settimeout(0.2)

    nonce = int(time.time() * 1000) & 0x7FFFFFFFFFFFFFFF
    targets = args.to or broadcast_targets()
    print(f'bound {"35109 (host candidate)" if owns else "ephemeral (join candidate)"}'
          f'  nonce={nonce:#x}')
    if not args.listen_only:
        print('sending hellos to: ' + ', '.join(targets))
    print('waiting for peers... (Ctrl-C to stop)\n')

    seen = {}
    next_hello = 0.0
    while True:
        now = time.time()
        if not args.listen_only and now >= next_hello:
            packet = struct.pack(FMT, MAGIC, 1, 0, 0, nonce, 0, 0, b'')
            for target in targets:
                try:
                    sock.sendto(packet, (target, PORT))
                except OSError as exc:
                    print(f'  send to {target} failed: {exc}')
            next_hello = now + 1.0
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) == LOG_SIZE and data.startswith(LOG_MAGIC):
            _, _, _, used, _, first, count, text = struct.unpack(LOG_FMT, data)
            if args.follow:
                print(f'  [log packet from {addr[0]} bytes={used} '
                      f'first={first} count={count}]')
                for offset, line in enumerate(
                        text[:used].decode('utf-8', 'replace').split('\n')):
                    if line:
                        print(f'{addr[0]} [{first + offset}] {line}')
            continue
        if len(data) != SIZE or not data.startswith(MAGIC):
            print(f'  {addr[0]}:{addr[1]} -> {len(data)} bytes, not a lobby packet')
            continue
        _, version, node, _, peer_nonce, games, seq, game = struct.unpack(FMT, data)
        if peer_nonce == nonce:
            continue
        key = (addr[0], peer_nonce)
        if key not in seen:
            print(f'PEER {addr[0]}:{addr[1]}  nonce={peer_nonce:#x} '
                  f'v{version} node={node} games={games:#x}')
            seen[key] = True


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\nstopped')
