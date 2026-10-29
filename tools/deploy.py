#!/usr/bin/env python3
"""Send one ELF to elfldr, using socket half-close on macOS and Linux."""
import argparse
import socket
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('host')
p.add_argument('port', type=int)
p.add_argument('elf', type=Path)
a = p.parse_args()
data = a.elf.read_bytes()
if not data.startswith(b'\x7fELF'):
    p.error('input is not an ELF')
with socket.create_connection((a.host, a.port), timeout=10) as sock:
    sock.sendall(data)
    sock.shutdown(socket.SHUT_WR)
print(f'Sent {len(data)} bytes to {a.host}:{a.port}')
