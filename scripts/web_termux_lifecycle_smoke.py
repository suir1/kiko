#!/usr/bin/env python3
"""Verify Termux Web wake-lock, browser-open, and shutdown cleanup ordering."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def write_command(path: Path, line: str) -> None:
    path.write_text("#!/bin/sh\n" + line + "\n", encoding="ascii")
    path.chmod(0o755)


def read_url(proc: subprocess.Popen[str]) -> str:
    deadline = time.time() + 8
    while time.time() < deadline:
        line = proc.stdout.readline() if proc.stdout else ""
        if not line:
            if proc.poll() is not None:
                fail(f"kiko web exited early with {proc.returncode}")
            time.sleep(0.05)
            continue
        match = re.search(r"(http://[^\s]+)", line)
        if match:
            return match.group(1)
    fail("kiko web did not print URL")
    return ""


def wait_for_events(log: Path, count: int) -> list[str]:
    deadline = time.time() + 5
    while time.time() < deadline:
        events = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
        if len(events) >= count:
            return events
        time.sleep(0.05)
    return log.read_text(encoding="utf-8").splitlines() if log.exists() else []


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: web_termux_lifecycle_smoke.py /path/to/kiko")

    kiko = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="kiko-termux-web-") as temp:
        root = Path(temp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        event_log = root / "events.log"
        write_command(fake_bin / "termux-wake-lock", 'printf "wake-lock\\n" >>"$KIKO_TERMUX_TEST_LOG"')
        write_command(fake_bin / "termux-wake-unlock", 'printf "wake-unlock\\n" >>"$KIKO_TERMUX_TEST_LOG"')
        write_command(
            fake_bin / "termux-open-url",
            'printf "open-url %s\\n" "$1" >>"$KIKO_TERMUX_TEST_LOG"',
        )

        env = os.environ.copy()
        env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
        env["TERMUX_VERSION"] = "test"
        env["PREFIX"] = str(root / "com.termux" / "usr")
        env["KIKO_TERMUX_TEST_LOG"] = str(event_log)

        proc = subprocess.Popen(
            [str(kiko), "web", "--listen", "127.0.0.1:0"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            url = read_url(proc)
            with urllib.request.urlopen(url, timeout=5) as response:
                if response.status != 200:
                    fail(f"Termux Web endpoint returned {response.status}")

            events = wait_for_events(event_log, 2)
            expected_open = f"open-url {url}"
            if events[:2] != ["wake-lock", expected_open]:
                fail(f"unexpected startup lifecycle events: {events!r}")

            proc.terminate()
            proc.wait(timeout=5)
            if proc.returncode != 0:
                fail(f"graceful Termux Web shutdown returned {proc.returncode}")

            events = wait_for_events(event_log, 3)
            if events != ["wake-lock", expected_open, "wake-unlock"]:
                fail(f"unexpected complete lifecycle events: {events!r}")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)

    print("Termux Web lifecycle smoke passed")


if __name__ == "__main__":
    main()
