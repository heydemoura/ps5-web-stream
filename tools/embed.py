#!/usr/bin/env python3
"""Embed a local browser asset as a deterministic C byte array."""
from pathlib import Path
import sys
source, target, symbol = sys.argv[1:]
data = Path(source).read_bytes()
lines = [f'static const unsigned char {symbol}[] = {{']
lines.extend('  ' + ','.join(f'0x{b:02x}' for b in data[i:i+16]) + ',' for i in range(0, len(data), 16))
lines += ['};', f'static const unsigned int {symbol}_len = sizeof({symbol});', '']
Path(target).parent.mkdir(parents=True, exist_ok=True)
Path(target).write_text('\n'.join(lines))
