#!/usr/bin/env python3
"""Smoke test for kiko web: boot server, verify page and token-gated API."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


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


def fetch(url: str) -> tuple[int, str]:
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            return response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: web_smoke.py /path/to/kiko")
    kiko = Path(sys.argv[1])
    proc = subprocess.Popen(
        [str(kiko), "web", "--no-open", "--listen", "127.0.0.1:0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        url = read_url(proc)
        if "token=" not in url:
            fail("URL missing token")
        base, query = url.split("?", 1)
        base = base.rstrip("/")

        status, body = fetch(base + "/")
        if status != 200 or "kiko web" not in body:
            fail("index page did not render")
        if "Notepad" not in body or "/api/note/start" not in body:
            fail("index page missing notepad UI")

        status, _ = fetch(base + "/api/config")
        if status != 401:
            fail("API without token should return 401")

        status, body = fetch(base + "/api/config?" + query)
        if status != 200:
            fail("config API with token failed")
        config = json.loads(body)
        if "relay" not in config or "listen" not in config:
            fail("config API missing expected fields")
        if "shortcuts" not in config or not isinstance(config["shortcuts"], list):
            fail("config API missing browser shortcuts")

        status, body = fetch(base + "/api/fs?path=.&mode=file_or_dir&sort=name&" + query)
        if status != 200:
            fail("fs API with token failed")
        listing = json.loads(body)
        if "entries" not in listing or not isinstance(listing["entries"], list):
            fail("fs API missing entries")

        status, body = fetch(base + "/api/job?" + query)
        if status != 200:
            fail("job API with token failed")
        job = json.loads(body)
        if job.get("running") is not False:
            fail("initial job should be idle")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    print("web smoke passed")


if __name__ == "__main__":
    main()
