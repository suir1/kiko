#!/usr/bin/env python3
import json
import socket
import struct
import subprocess
import sys
import time
import re


def send_msg(sock, msg):
    data = json.dumps(msg, separators=(",", ":")).encode()
    sock.sendall(b"kiko" + struct.pack("!I", len(data)) + data)


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"relay closed with {size - len(data)} bytes left")
        data.extend(chunk)
    return bytes(data)


def recv_msg(sock):
    header = recv_exact(sock, 8)
    if header[:4] != b"kiko":
        raise RuntimeError(f"bad frame header: {header!r}")
    size = struct.unpack("!I", header[4:])[0]
    return json.loads(recv_exact(sock, size).decode())


def relay_version(binary):
    proc = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=3)
    if proc.returncode != 0:
        raise RuntimeError(f"relayd --version failed: {proc.stderr.strip()}")
    match = re.fullmatch(r"kiko-relayd (\S+)\s*", proc.stdout)
    if not match:
        raise RuntimeError(f"unexpected relayd version output: {proc.stdout!r}")
    return match.group(1)


def main():
    if len(sys.argv) != 2:
        print("usage: relayd_smoke.py /path/to/kiko-relayd", file=sys.stderr)
        return 2

    try:
        expected_version = relay_version(sys.argv[1])
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 1

    proc = subprocess.Popen(
        [sys.argv[1], "--listen", "127.0.0.1:0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        line = proc.stdout.readline().strip()
        if "relay listening on " not in line:
            print(f"unexpected relayd output: {line!r}", file=sys.stderr)
            return 1
        port = int(line.rsplit(":", 1)[1])
        deadline = time.time() + 3
        last_error = None
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                    send_msg(sock, {"type": "ping"})
                    msg = recv_msg(sock)
                    if msg.get("type") != "pong":
                        raise RuntimeError(f"expected pong, got {msg!r}")
                    if msg.get("version") != expected_version:
                        raise RuntimeError(
                            f"relay pong version mismatch: expected {expected_version!r}, got {msg!r}"
                        )
                    return 0
            except OSError as error:
                last_error = error
                time.sleep(0.05)
        print(f"failed to connect to relayd: {last_error}", file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
