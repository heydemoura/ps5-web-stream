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


def check_frame(url, tmp):
    started = time.monotonic()
    with urllib.request.urlopen(url, timeout=5) as response:
        headers = response.headers
        body = response.read()
    assert time.monotonic() - started < 3
    assert headers["Content-Type"] == "video/h264", headers
    assert headers["X-Frame-Width"] == "320" and headers["X-Frame-Height"] == "180", headers
    # Frames older than 500 ms are discarded and replaced, never served.
    assert int(headers["X-Frame-Age-Ms"]) <= 600, headers
    assert int(headers["Content-Length"]) == len(body)
    assert body.startswith(b"\0\0\0\1\x67"), body[:8]
    # The single standalone keyframe must decode to exactly one image.
    image = Path(tmp) / "frame.png"
    result = subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "h264",
        "-i", "pipe:0", "-frames:v", "1", str(image)
    ], input=body, capture_output=True, timeout=20, check=True)
    assert not result.stderr, result.stderr
    probe = subprocess.run([
        ffmpeg, "-hide_banner", "-f", "h264", "-i", "pipe:0", "-f", "framehash", "-"
    ], input=body, capture_output=True, timeout=20, check=True)
    frames = [line for line in probe.stdout.decode().splitlines()
              if line and not line.startswith("#")]
    assert len(frames) == 1, probe.stdout
    assert image.stat().st_size > 1000
    return int(headers["X-Frame-Timestamp-Ms"])


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
        "-g", "15", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2",
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
            frame_url = f"http://127.0.0.1:{http_port}/frame.h264"
            first = check_frame(frame_url, tmp)
            time.sleep(1.2)
            assert check_frame(frame_url, tmp) > first, "frame did not advance"
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
            print(f"HTTP-FLV simultaneous viewers, late join and /frame.h264 keyframe decode passed: {results}")
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
