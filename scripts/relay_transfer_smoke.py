#!/usr/bin/env python3
"""End-to-end smoke test: relay + CLI send/recv + content check."""

from __future__ import annotations

import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_KIKO = ROOT / "build" / "kiko"


def fail(message: str, *logs: tuple[str, str]) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    for title, text in logs:
        if text:
            print(f"---- {title} ----", file=sys.stderr)
            print(text[-6000:], file=sys.stderr)
    sys.exit(1)


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_port(port: int, timeout: float = 8.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def terminate(proc: subprocess.Popen[str] | None) -> str:
    if proc is None:
        return ""
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    stdout, stderr = proc.communicate()
    return (stdout or "") + (stderr or "")


def parse_args(argv: list[str]) -> tuple[Path, str | None]:
    kiko = Path(os.environ.get("KIKO_BIN", DEFAULT_KIKO))
    relay: str | None = None
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--relay":
            if i + 1 >= len(argv):
                fail("--relay requires HOST:PORT")
            relay = argv[i + 1]
            i += 2
        elif arg.startswith("--"):
            fail(f"unknown option: {arg}")
        else:
            kiko = Path(arg)
            i += 1
    return kiko, relay


def run_pair(kiko: Path, work: Path, relay: str) -> None:
    source_dir = work / "source"
    recv_dir = work / "received"
    source_dir.mkdir()
    recv_dir.mkdir()

    source_file = source_dir / "payload.txt"
    source_file.write_text(
        "kiko relay transfer smoke\n"
        + "0123456789abcdef" * 4096
        + "\n",
        encoding="utf-8",
    )

    code = f"smoke{os.getpid()}{int(time.time())}"
    env = os.environ.copy()
    env["KIKO_RELAY"] = relay

    recv_cmd = [
        str(kiko),
        "recv",
        code,
        "--relay",
        relay,
        "--out",
        str(recv_dir),
        "--no-direct",
        "--no-lan",
        "--no-local",
    ]
    send_cmd = [
        str(kiko),
        "send",
        str(source_file),
        "--relay",
        relay,
        "--code",
        code,
        "--no-direct",
        "--no-lan",
        "--no-local",
        "--no-qrcode",
        "--connections",
        "2",
    ]

    recv_proc = subprocess.Popen(
        recv_cmd,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(0.5)
    if recv_proc.poll() is not None:
        recv_log = terminate(recv_proc)
        fail("receiver exited before sender started", ("receiver", recv_log))

    send_result = subprocess.run(
        send_cmd,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=45,
    )
    if send_result.returncode != 0:
        recv_log = terminate(recv_proc)
        fail(
            f"sender failed with exit code {send_result.returncode}",
            ("sender stdout", send_result.stdout),
            ("sender stderr", send_result.stderr),
            ("receiver", recv_log),
        )

    try:
        recv_stdout, recv_stderr = recv_proc.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        recv_log = terminate(recv_proc)
        fail("receiver did not finish", ("sender stdout", send_result.stdout), ("receiver", recv_log))
    if recv_proc.returncode != 0:
        fail(
            f"receiver failed with exit code {recv_proc.returncode}",
            ("sender stdout", send_result.stdout),
            ("sender stderr", send_result.stderr),
            ("receiver stdout", recv_stdout),
            ("receiver stderr", recv_stderr),
        )

    received_file = recv_dir / source_file.name
    if not received_file.is_file():
        fail(
            "received file missing",
            ("sender stdout", send_result.stdout),
            ("receiver stdout", recv_stdout),
        )
    if sha256(source_file) != sha256(received_file):
        fail("received file hash mismatch")

    combined = "\n".join([send_result.stdout, send_result.stderr, recv_stdout, recv_stderr])
    if "pake handshake ok" not in combined:
        fail("transfer completed without expected PAKE confirmation", ("transfer logs", combined))
    print("relay transfer smoke passed")


def main() -> None:
    kiko, external_relay = parse_args(sys.argv)
    if not kiko.is_file():
        fail(f"kiko binary not found: {kiko}")

    work = Path(tempfile.mkdtemp(prefix="kiko-relay-transfer-smoke-"))
    relay_proc: subprocess.Popen[str] | None = None
    try:
        relay = external_relay
        if relay is None:
            port = free_tcp_port()
            relay = f"127.0.0.1:{port}"
            relay_proc = subprocess.Popen(
                [str(kiko), "relay", "--listen", relay],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if not wait_for_port(port):
                relay_log = terminate(relay_proc)
                fail(f"relay did not listen on {relay}", ("relay", relay_log))
        run_pair(kiko, work, relay)
    finally:
        terminate(relay_proc)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
