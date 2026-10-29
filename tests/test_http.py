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
    assert data["video"] == "1920x1080" and data["rtmp_port"] == 1935, data
    status, _, body = request("/")
    assert status == 200 and b'/vendor/flv.min.js' in body
    assert b"cdnjs" not in body
    status, headers, player = request("/vendor/flv.min.js")
    assert status == 200 and len(player) > 100000
    assert headers[b"content-type"] == b"application/javascript"
    status, _, license_body = request("/vendor/flv.LICENSE")
    assert status == 200 and b"Apache License" in license_body
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
