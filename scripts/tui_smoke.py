#!/usr/bin/env python3
"""PTY smoke tests for kiko TUI (menu render + send --tui progress screen)."""

from __future__ import annotations

import fcntl
import json
import os
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_KIKO = ROOT / "build" / "kiko"
TEST_FILE = ROOT / "README.md"


def set_winsize(fd: int, rows: int, cols: int) -> None:
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)


def strip_ansi(data: bytes) -> str:
    text = data.decode("utf-8", errors="replace")
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", text)
    text = re.sub(r"\x1b\][^\x07]*(?:\x07|\x1b\\)", "", text)
    return text


def read_available(fd: int, timeout: float = 2.0) -> bytes:
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.15)
        if fd in ready:
            chunk = os.read(fd, 16384)
            if not chunk:
                break
            out += chunk
        elif out:
            break
    return out


def spawn(argv: list[str], env: dict[str, str] | None = None) -> tuple[subprocess.Popen[bytes], int]:
    master, slave = pty.openpty()
    set_winsize(slave, 45, 130)
    proc = subprocess.Popen(
        argv,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        cwd=ROOT,
        env=env,
        close_fds=True,
    )
    os.close(slave)
    return proc, master


def stop(proc: subprocess.Popen[bytes], master: int) -> None:
    try:
        if proc.poll() is None:
            try:
                os.write(master, b"q")
                time.sleep(0.3)
                os.write(master, b"\x1b")
                time.sleep(0.3)
            except OSError:
                pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
    finally:
        os.close(master)


def fail(msg: str, blob: bytes = b"") -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    if blob:
        print(strip_ansi(blob)[-4000:], file=sys.stderr)
    sys.exit(1)


def ok(msg: str) -> None:
    print(f"OK: {msg}")


def test_menu(kiko: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kiko_tui_smoke_") as temp:
        browse_dir = Path(temp) / "browse"
        browse_dir.mkdir()
        fixture = browse_dir / "browser-visible.txt"
        fixture.write_text("visible\n", encoding="utf-8")
        clean_path = str(fixture)
        broken_path = clean_path[:3] + "\n" + clean_path[3:]
        config = Path(temp) / "config.json"
        config.write_text(json.dumps({"last_mode": 0, "last_send_path": broken_path}), encoding="utf-8")
        env = os.environ.copy()
        env["KIKO_CONFIG_PATH"] = str(config)

        proc, master = spawn([str(kiko), "tui"], env=env)
        time.sleep(1.2)
        out = read_available(master, 1.5)
        plain = strip_ansi(out)
        if "kiko" not in plain.lower() or "Start" not in plain:
            stop(proc, master)
            fail("kiko tui menu did not render", out)
        for label in ("File...", "Folder...", "Browse..."):
            if label not in plain:
                stop(proc, master)
                fail(f"kiko tui menu missing {label} picker", out)
        if proc.poll() is not None:
            fail("kiko tui exited before quit", out)

        os.write(master, b"\x1b[<0;32;10M\x1b[<0;32;10m")
        time.sleep(0.8)
        browser_out = read_available(master, 1.5)
        browser_plain = strip_ansi(browser_out)
        if "Select path" not in browser_plain or fixture.name not in browser_plain:
            stop(proc, master)
            fail("built-in TUI browser did not open with repaired saved path", out + browser_out)

        os.write(master, b"q")
        time.sleep(0.4)
        read_available(master, 0.5)
        os.write(master, b"\x1b[Z\x1b[Z\x1b[Z\r")
        time.sleep(0.8)
        enter_out = read_available(master, 1.5)
        enter_plain = strip_ansi(enter_out)
        if "Select path" not in enter_plain or fixture.name not in enter_plain:
            stop(proc, master)
            fail("Enter on the path field did not open the built-in browser", enter_out)

        stop(proc, master)
        ok("kiko tui menu and built-in browser render")


def test_send_tui(kiko: Path) -> None:
    proc, master = spawn(
        [
            str(kiko),
            "send",
            str(TEST_FILE),
            "--tui",
            "--relay",
            "127.0.0.1:9000",
            "--no-lan",
        ]
    )
    time.sleep(2.5)
    out = read_available(master, 2.0)
    plain = strip_ansi(out)
    needles = ("kiko send", "pairing code", "connectivity", "listening", "starting", "waiting")
    if not any(n in plain for n in needles):
        stop(proc, master)
        fail("send --tui progress screen did not render", out)
    if proc.poll() is not None:
        fail("send --tui exited unexpectedly", out)
    stop(proc, master)
    ok("send --tui progress screen renders")


def main() -> None:
    kiko = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(os.environ.get("KIKO_BIN", DEFAULT_KIKO))
    if not kiko.is_file():
        fail(f"kiko binary not found: {kiko}")
    if not TEST_FILE.is_file():
        fail(f"test file not found: {TEST_FILE}")

    test_menu(kiko)
    test_send_tui(kiko)
    print("tui smoke passed")


if __name__ == "__main__":
    main()
