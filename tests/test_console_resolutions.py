#!/usr/bin/env python3
"""Opt-in hardware check: switches every capture mode and decodes both tracks.

Usage: python3 tests/test_console_resolutions.py http://PS5:8081 /tmp/ps5hs-modes
The output directory contains captured console media; keep it outside version control.
The console returns to 1080p when the check finishes, including on failure.
"""
import json
from pathlib import Path
import subprocess
import sys
import time
import urllib.request

base = sys.argv[1].rstrip("/")
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
modes = {"4k": (3840, 2160), "1440p": (2560, 1440),
         "720p": (1280, 720), "1080p": (1920, 1080)}


def status():
    with urllib.request.urlopen(base + "/status", timeout=5) as response:
        return json.load(response)


def select(mode):
    request = urllib.request.Request(base + "/settings",
        data=("resolution=" + mode).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded", "X-PS5HS-Request": "1"})
    with urllib.request.urlopen(request, timeout=5) as response:
        assert response.status == 202
        revision = json.load(response)["settings_revision"]
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        state = status()
        if state["capture_state"] == "streaming" and state["active_resolution"] == mode:
            return state
        if state["capture_state"] == "error" and state.get("capture_revision") == revision:
            raise AssertionError(f"{mode}: capture error {state['error'] & 0xffffffff:#x}")
        time.sleep(.2)
    raise AssertionError(f"{mode}: did not activate: {state}")


results = {}
deadline = time.monotonic() + 20
while True:
    try:
        status()
        break
    except OSError:
        if time.monotonic() > deadline:
            raise
        time.sleep(.2)
try:
    for mode, dimensions in modes.items():
        try:
            before = select(mode)
            # Packet timestamps alone can lie about delivery cadence: native auto
            # mode has produced 30 frames/wall-second with nominal 60fps DTS.
            sample_start = time.monotonic()
            sample_before = status()
            time.sleep(5)
            sample_after = status()
            wall_fps = (sample_after['video_frames'] - sample_before['video_frames']) / (time.monotonic() - sample_start)
            assert sample_after['active_resolution'] == mode and 58 <= wall_fps <= 61, (mode, wall_fps)
            media = out / (mode + ".flv")
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y",
                "-rw_timeout", "10000000", "-i", base + "/live.flv", "-t", "6",
                "-map", "0:v:0", "-map", "0:a:0", "-c", "copy", str(media)],
                check=True, timeout=25)
            probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error",
                "-count_frames", "-show_entries",
                "stream=codec_name,width,height,sample_rate,channels,nb_read_frames:format=duration",
                "-of", "json", str(media)], timeout=20))
            video, audio = probe["streams"]
            assert (video["width"], video["height"]) == dimensions, probe
            assert video["codec_name"] == "h264" and int(video["nb_read_frames"]) > 340, probe
            assert audio["codec_name"] == "aac" and audio["channels"] == 2, probe
            assert int(audio.get("nb_read_frames", 0)) > 50, probe
            packets = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_packets",
                "-show_entries", "packet=stream_index,dts_time", "-of", "json", str(media)],
                timeout=20))["packets"]
            for track in (0, 1):
                times = [float(packet["dts_time"]) for packet in packets if packet["stream_index"] == track]
                assert all(a < b for a, b in zip(times, times[1:])), (mode, track, times)
            video_times = [float(p["dts_time"]) for p in packets if p["stream_index"] == 0]
            measured_fps = (len(video_times) - 1) / (video_times[-1] - video_times[0])
            assert 58 <= measured_fps <= 61, (mode, measured_fps)
            decoded = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(media),
                "-map", "0:v:0", "-map", "0:a:0", "-enc_time_base:v", "1:1000",
                "-fps_mode", "passthrough", "-f", "null", "-"],
                capture_output=True, text=True, timeout=30)
            assert decoded.returncode == 0 and not decoded.stderr, decoded.stderr
            results[mode] = {"passed": True, "status": before, "media": probe,
                             "wall_fps": wall_fps, "measured_fps": measured_fps, "monotonic_dts": True, "decode_errors": ""}
        except (AssertionError, OSError, subprocess.SubprocessError) as error:
            results[mode] = {"passed": False, "error": str(error)}
        print(mode, json.dumps(results[mode]), flush=True)
finally:
    try:
        select("1080p")
    finally:
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
assert len(results) == 4 and all(result["passed"] for result in results.values()), results
