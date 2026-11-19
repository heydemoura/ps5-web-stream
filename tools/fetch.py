#!/usr/bin/env python3
"""Download a pinned source archive and verify its SHA-256 before use."""
import hashlib
import os
import sys
import urllib.request

url, expected, out = sys.argv[1:4]
tmp = out + ".part"
with urllib.request.urlopen(url, timeout=120) as response, open(tmp, "wb") as f:
    digest = hashlib.sha256()
    while chunk := response.read(1 << 20):
        digest.update(chunk)
        f.write(chunk)
if digest.hexdigest() != expected:
    os.unlink(tmp)
    sys.exit(f"{url}: SHA-256 {digest.hexdigest()} != pinned {expected}")
os.replace(tmp, out)
print(f"Fetched and verified {out}")
