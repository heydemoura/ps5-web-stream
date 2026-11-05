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


def fetch(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return response.headers, response.read()


def psnr(reference, candidate):
    # Compare colour, not edge detail: area-downscale both in RGB, letting FFmpeg
    # honour each file's signalled matrix and range. On this pattern a correct
    # conversion scores 31-33 dB; a BT.601-for-BT.709 or range mistake ~23 dB.
    result = subprocess.run([
        ffmpeg, "-hide_banner", "-i", str(reference), "-i", str(candidate),
        "-lavfi", "[0:v]scale=40:22:flags=area,format=rgb24[a];"
        "[1:v]scale=40:22:flags=area,format=rgb24[b];[a][b]psnr",
        "-f", "null", "-"
    ], capture_output=True, text=True, timeout=20, check=True)
    line = [l for l in result.stderr.splitlines() if "PSNR" in l][-1]
    return float(line.split("average:")[1].split()[0])


def check_jpeg(base, tmp):
    for _ in range(5):
        # Pair a JPEG with the H.264 keyframe it was converted from.
        jpeg_headers, jpeg = fetch(base + "/frame.jpeg")
        h264_headers, h264 = fetch(base + "/frame.h264")
        if jpeg_headers["X-Frame-Timestamp-Ms"] == h264_headers["X-Frame-Timestamp-Ms"]:
            break
    else:
        raise AssertionError("could not fetch a matching JPEG/H.264 pair")
    assert jpeg_headers["Content-Type"] == "image/jpeg", jpeg_headers
    assert jpeg_headers["Content-Disposition"].startswith("attachment;"), jpeg_headers
    assert jpeg_headers["X-Frame-Width"] == "320", jpeg_headers
    assert jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
    # The conversion is cached per keyframe.
    again_headers, again = fetch(base + "/frame.jpeg")
    if again_headers["X-Frame-Timestamp-Ms"] == jpeg_headers["X-Frame-Timestamp-Ms"]:
        assert again == jpeg
    jpeg_path, h264_path = Path(tmp) / "frame.jpg", Path(tmp) / "pair.h264"
    jpeg_path.write_bytes(jpeg); h264_path.write_bytes(h264)
    probe = json.loads(subprocess.run([
        ffmpeg.replace("ffmpeg", "ffprobe"), "-v", "error", "-show_entries",
        "stream=codec_name,width,height,pix_fmt,color_range", "-of", "json", str(jpeg_path)
    ], capture_output=True, text=True, check=True).stdout)["streams"][0]
    assert probe["codec_name"] == "mjpeg" and (probe["width"], probe["height"]) == (320, 180), probe
    assert probe["pix_fmt"] == "yuvj420p" or probe.get("color_range") == "pc", probe
    # Wrong matrix or range would shift colours far below this threshold.
    score = psnr(h264_path, jpeg_path)
    assert score > 28, f"JPEG colours diverge from the source frame: PSNR {score:.1f} dB"
    return score


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
        "-g", "15", "-pix_fmt", "yuv420p",
        # Signal BT.709 limited range, as the console's encoder does.
        "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-color_range", "tv", "-c:a", "aac", "-ac", "2",
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
            jpeg_psnr = check_jpeg(f"http://127.0.0.1:{http_port}", tmp)
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
            print(f"HTTP-FLV simultaneous viewers, late join, /frame.h264 decode and /frame.jpeg "
                  f"(PSNR {jpeg_psnr:.1f} dB vs. source) passed: {results}")
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
