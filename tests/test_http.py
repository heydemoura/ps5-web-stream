#!/usr/bin/env python3
"""Exercise the actual HTTP server with fragmented and disconnected sockets.

Usage: python3 tests/test_http.py path/to/http-fixture
The fixture is linked with http.c, flv.c and status.c (see Makefile).
"""
import json
import socket
import subprocess
import sys
import time


def connect():
    return socket.create_connection(("127.0.0.1", port), timeout=8)


def receive(sock):
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            if not chunks:
                raise
            return b"".join(chunks)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def request(path, method="GET", fragments=False):
    with connect() as sock:
        data = f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode()
        if fragments:
            for chunk in (data[:2], data[2:9], data[9:-1], data[-1:]):
                sock.sendall(chunk)
                time.sleep(.02)
        else:
            sock.sendall(data)
        response = receive(sock)
    head, body = response.split(b"\r\n\r\n", 1)
    status = int(head.split()[1])
    headers = {key.lower(): value for key, value in (line.split(b": ", 1) for line in head.split(b"\r\n")[1:])}
    assert int(headers[b"content-length"]) == len(body), head
    return status, headers, body


with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
process = subprocess.Popen([sys.argv[1], str(port)], stdout=subprocess.DEVNULL)
try:
    for _ in range(100):
        if process.poll() is not None:
            raise AssertionError("Fixture exited during startup")
        try:
            with connect():
                break
        except OSError:
            time.sleep(.02)
    else:
        raise AssertionError("Fixture did not start")

    status, headers, body = request("/status?fresh=1", fragments=True)
    data = json.loads(body)
    assert status == 200 and data["capture_state"] == "error", data
    assert data["video_frames"] == 0 and data["audio"] is False, data
    assert data["video"] == "1920x1080" and "rtmp_port" not in data, data
    status, _, body = request("/")
    assert status == 200 and b'/vendor/flv.min.js' in body
    assert b"cdnjs" not in body
    status, _, body = request("/player")
    assert status == 200 and b'autoplay muted playsinline' in body
    assert b'data-player="fullscreen"' in body and b'/app.js' in body
    assert request("/player/")[0] == 200 and request("/player-extra")[0] == 404
    status, _, body = request("/app.js")
    assert status == 200 and b'capture_state' in body and b'rtmp' not in body.lower()
    status, headers, player = request("/vendor/flv.min.js")
    assert status == 200 and len(player) > 100000
    assert headers[b"content-type"] == b"application/javascript"
    status, _, license_body = request("/vendor/flv.LICENSE")
    assert status == 200 and b"Apache License" in license_body
    status, headers, body = request("/vendor/videojs.min.js")
    assert status == 200 and len(body) > 50000
    assert headers[b"content-type"] == b"application/javascript"
    assert request("/vendor/videojs.LICENSE")[0] == 200
    status, headers, body = request("/player.css")
    assert status == 200 and headers[b"content-type"] == b"text/css"
    assert b".player-controls" in body
    status, _, body = request("/log")
    assert status == 200 and b"http fixture ready" in body
    for path in ("/indexgarbage", "/live.flvgarbage", "/status/x", "/../status"):
        assert request(path)[0] == 404, path
    status, headers, _ = request("/", "POST")
    assert status == 405 and headers[b"allow"] == b"GET"
    with connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\x00\r\n\r\n")
        assert receive(sock).startswith(b"HTTP/1.1 400")
    with connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nX-Huge: " + b"a" * 8200)
        response = sock.recv(8192)
        assert response.startswith(b"HTTP/1.1 431"), response

    # Settings requests accept only exact supported presets, with bounded bodies.
    def settings_request(value, custom=True, fragments=False, length=None, extra=b""):
        body = ("resolution=" + value).encode()
        n = len(body) if length is None else length
        headers = (b"POST /settings HTTP/1.1\r\nHost: localhost\r\n"
                   b"Content-Type: application/x-www-form-urlencoded\r\n" +
                   (b"X-PS5HS-Request: 1\r\n" if custom else b"") +
                   f"Content-Length: {n}\r\n".encode() + extra + b"\r\n")
        with connect() as sock:
            sock.sendall(headers)
            for part in ([body[:5], body[5:]] if fragments else [body]):
                sock.sendall(part)
                if fragments: time.sleep(.03)
            return receive(sock)
    assert json.loads(request("/settings")[2])["requested_resolution"] == "1080p"
    revision = data["settings_revision"]
    for mode in ("4k", "1440p", "720p", "1080p"):
        response = settings_request(mode, fragments=True)
        assert response.startswith(b"HTTP/1.1 202"), response
        updated = json.loads(response.split(b"\r\n\r\n", 1)[1])
        assert updated["requested_resolution"] == mode and updated["applying"] is True
        assert updated["active_resolution"] is None
        assert updated["settings_revision"] > revision
        revision = updated["settings_revision"]
    assert settings_request("1080p").startswith(b"HTTP/1.1 202")
    assert json.loads(request("/settings")[2])["settings_revision"] == revision
    for invalid in ("8k", "1080p&ignored=true", "", "720p%00", "720p\0"):
        assert settings_request(invalid).startswith(b"HTTP/1.1 400")
    assert settings_request("720p", custom=False).startswith(b"HTTP/1.1 403")
    assert settings_request("720p", length=65).startswith(b"HTTP/1.1 413")
    assert settings_request("720p", extra=b"Content-Length: 16\r\n").startswith(b"HTTP/1.1 400")
    assert settings_request("720p", extra=b"Transfer-Encoding: chunked\r\n").startswith(b"HTTP/1.1 400")
    assert json.loads(request("/settings")[2])["requested_resolution"] == "1080p"

    # Eight slots, then a real HTTP error; closing idle viewers releases slots
    # even though no capture frame has ever arrived.
    streams = []
    try:
        for _ in range(8):
            sock = connect()
            streams.append(sock)
            sock.sendall(b"GET /live.flv HTTP/1.1\r\nHost: localhost\r\n\r\n")
            response = sock.recv(4096)
            assert response.startswith(b"HTTP/1.1 200"), response
        assert request("/live.flv")[0] == 503
        assert json.loads(request("/status")[2])["clients"] == 8
    finally:
        for sock in streams:
            sock.close()
    deadline = time.monotonic() + 4
    while json.loads(request("/status")[2])["clients"]:
        assert time.monotonic() < deadline, "Disconnected viewers leaked slots"
        time.sleep(.05)

    idle = []
    try:
        for _ in range(24):
            sock = connect()
            sock.sendall(b"GET /status HTTP/1.1\r\n")
            idle.append(sock)
        assert request("/status")[0] == 503, "HTTP worker limit must be enforced"
    finally:
        for sock in idle:
            sock.close()
    deadline = time.monotonic() + 2
    while request("/status")[0] != 200:
        assert time.monotonic() < deadline
        time.sleep(.02)

    with connect() as sock:
        started = time.monotonic()
        sock.sendall(b"GET /status HTTP/1.1\r\n")
        # Incomplete requests must not occupy a thread indefinitely.
        assert receive(sock).startswith(b"HTTP/1.1 408")
        assert time.monotonic() - started < 7
    assert request("/status")[0] == 200
    print("HTTP integration: fragmented requests, truthful status, local player, exact routes, capacity and idle disconnects passed")
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
