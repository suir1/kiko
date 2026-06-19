#!/usr/bin/env python3
import json
import socket
import struct
import subprocess
import sys
import time


def send_msg(sock, msg):
    data = json.dumps(msg, separators=(",", ":")).encode()
    sock.sendall(b"kiko" + struct.pack("!I", len(data)) + data)


def recv_msg(sock):
    header = sock.recv(8)
    if len(header) != 8 or header[:4] != b"kiko":
        raise RuntimeError(f"bad frame header: {header!r}")
    size = struct.unpack("!I", header[4:])[0]
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("relay closed frame")
        data.extend(chunk)
    return json.loads(data.decode())


def main():
    if len(sys.argv) != 2:
        print("usage: relayd_smoke.py /path/to/kiko-relayd", file=sys.stderr)
        return 2

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
