#!/usr/bin/env python3
"""Smoke test for configurable pairing timeout."""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def terminate(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: pair_timeout_smoke.py /path/to/kiko /path/to/kiko-relayd")

    kiko = Path(sys.argv[1])
    relayd = Path(sys.argv[2])
    relay_addr = f"127.0.0.1:{free_tcp_port()}"
    relay = subprocess.Popen(
        [str(relayd), "--listen", relay_addr],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        time.sleep(0.2)
        if relay.poll() is not None:
            output = relay.stdout.read() if relay.stdout else ""
            fail("kiko-relayd exited early: " + output)

        start = time.monotonic()
        proc = subprocess.run(
            [
                str(kiko),
                "note",
                "host",
                "--relay",
                relay_addr,
                "--no-direct",
                "--no-lan",
                "--no-local",
                "--no-qrcode",
                "--pair-timeout",
                "1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=8,
        )
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            fail("note host unexpectedly succeeded without a peer")
        if elapsed > 4:
            fail(f"pair timeout took too long: {elapsed:.2f}s")
        if "failed to connect relay or rendezvous peer" not in proc.stderr:
            fail("unexpected stderr: " + proc.stderr)
    finally:
        terminate(relay)

    print("pair timeout smoke passed")


if __name__ == "__main__":
    main()
