#!/usr/bin/env python3
"""Decode real H.264/AAC through HTTP-FLV using a host fixture server.

Usage: python3 tests/test_streaming.py path/to/transport-fixture-server
Requires ffmpeg with libx264 and AAC. This validates transport, not PS5 capture.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def decode(url):
    result = subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-rw_timeout", "5000000",
        "-i", url, "-map", "0:v:0", "-map", "0:a:0", "-t", "2",
        "-f", "framehash", "-"
    ], capture_output=True, text=True, timeout=20, check=True)
    assert not result.stderr, result.stderr
    frames = {"0": 0, "1": 0}
    for line in result.stdout.splitlines():
        if not line.startswith("#") and "," in line:
            frames[line.split(",", 1)[0].strip()] += 1
    assert frames["0"] >= 50 and frames["1"] >= 80, frames
    assert "#dimensions 0: 320x180" in result.stdout
    return frames


ffmpeg = shutil.which("ffmpeg")
if not ffmpeg:
    raise SystemExit("ffmpeg is required for the streaming integration test")
http_port = available_port()
with tempfile.TemporaryDirectory(prefix="ps5hs-stream-test-") as tmp:
    fixture = Path(tmp) / "fixture.flv"
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30",
        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
        "-t", "5", "-c:v", "libx264", "-preset", "ultrafast",
        "-g", "30", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2",
        "-b:a", "128k", "-f", "flv", str(fixture)
    ], check=True, timeout=20)
    log_path = Path(tmp) / "server.log"
    with log_path.open("w+") as log:
        server = subprocess.Popen([
            sys.argv[1], str(fixture), str(http_port)
        ], stdout=log, stderr=log)
        try:
            status_url = f"http://127.0.0.1:{http_port}/status"
            for _ in range(100):
                if server.poll() is not None:
                    raise AssertionError("fixture exited at startup")
                try:
                    with urllib.request.urlopen(status_url, timeout=1) as response:
                        json.load(response)
                    break
                except OSError:
                    time.sleep(.02)
            else:
                raise AssertionError("fixture did not start")
            urls = [f"http://127.0.0.1:{http_port}/live.flv"] * 2
            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(decode, urls))
            # Late join after the fixture loops must also receive codec headers.
            results.append(decode(urls[0]))
            for _ in range(40):
                with urllib.request.urlopen(status_url, timeout=1) as response:
                    clients = json.load(response)["clients"]
                if clients == 0:
                    break
                time.sleep(.1)
            assert clients == 0, clients
            assert server.poll() is None
            print(f"HTTP-FLV simultaneous viewers and late join H.264 + stereo AAC decode passed: {results}")
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
            log.seek(0)
            output = log.read()
            assert "ERROR: AddressSanitizer" not in output, output
            assert "runtime error:" not in output, output
