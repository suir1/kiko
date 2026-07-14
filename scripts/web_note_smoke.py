#!/usr/bin/env python3
"""Smoke test for Web Notepad over a local relay."""

from __future__ import annotations

import json
import re
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def api(base: str, path: str, body: dict | None = None) -> dict:
    data = None
    headers: dict[str, str] = {}
    method = "GET"
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["content-type"] = "application/json"
        method = "POST"
    request = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=5) as response:
        text = response.read().decode("utf-8")
        return json.loads(text) if text else {}


def read_web_url(proc: subprocess.Popen[str]) -> str:
    deadline = time.time() + 8
    while time.time() < deadline:
        line = proc.stdout.readline() if proc.stdout else ""
        match = re.search(r"(http://[^\s]+)", line)
        if match:
            return match.group(1)
        if proc.poll() is not None:
            fail(f"kiko web exited early with {proc.returncode}")
        time.sleep(0.05)
    fail("kiko web did not print URL")
    return ""


def terminate(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def dump_child_output(procs: list[subprocess.Popen[str]]) -> None:
    for proc in procs:
        if not proc.stdout:
            continue
        output = proc.stdout.read()
        if not output:
            continue
        print(f"--- child output: {proc.args} ---", file=sys.stderr)
        print(output[-12000:], file=sys.stderr)


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def communicate_or_fail(proc: subprocess.Popen[str], timeout: float) -> str:
    try:
        output, _ = proc.communicate(timeout=timeout)
        return output or ""
    except subprocess.TimeoutExpired as exc:
        partial = exc.output or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "replace")
        terminate(proc)
        fail("join did not exit after /quit:\n" + partial[-2000:])
        return ""


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: web_note_smoke.py /path/to/kiko /path/to/kiko-relayd")

    kiko = Path(sys.argv[1])
    relayd = Path(sys.argv[2])
    relay_addr = f"127.0.0.1:{free_tcp_port()}"
    procs: list[subprocess.Popen[str]] = []

    try:
        relay = subprocess.Popen(
            [str(relayd), "--listen", relay_addr],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(relay)
        time.sleep(0.2)
        if relay.poll() is not None:
            fail("kiko-relayd exited early")

        web = subprocess.Popen(
            [str(kiko), "web", "--no-open", "--listen", "127.0.0.1:0", "--relay", relay_addr],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(web)
        web_url = read_web_url(web)
        base, query = web_url.split("?", 1)
        base = base.rstrip("/")
        token_path = "?" + query

        api(
            base,
            "/api/note/start" + token_path,
            {
                "role": "host",
                "code": "",
                "relay": relay_addr,
                "no_direct": True,
                "no_lan": True,
                "no_local": True,
            },
        )

        code = ""
        deadline = time.time() + 8
        while time.time() < deadline:
            job = api(base, "/api/job" + token_path)
            code = str(job.get("code") or "")
            if code:
                break
            time.sleep(0.1)
        if not code:
            fail("web notepad did not generate a code")

        join = subprocess.Popen(
            [
                str(kiko),
                "note",
                "join",
                code,
                "--relay",
                relay_addr,
                "--no-direct",
                "--no-lan",
                "--no-local",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(join)

        deadline = time.time() + 12
        while time.time() < deadline:
            job = api(base, "/api/job" + token_path)
            if job.get("note_connected"):
                break
            if join.poll() is not None:
                output = join.stdout.read() if join.stdout else ""
                fail("join exited before connect: " + output)
            time.sleep(0.1)
        else:
            fail("web notepad did not connect")

        note_text = "hello from web note"
        api(base, "/api/note/update" + token_path, {"text": note_text})
        deadline = time.time() + 8
        while time.time() < deadline:
            job = api(base, "/api/job" + token_path)
            if job.get("note_synced") and job.get("note_text") == note_text:
                break
            time.sleep(0.1)
        else:
            fail("web notepad did not receive ack")

        api(base, "/api/note/pad/create" + token_path, {})
        pad_text = "hello from web note pad two"
        api(base, "/api/note/update" + token_path, {"text": pad_text})
        deadline = time.time() + 8
        while time.time() < deadline:
            job = api(base, "/api/job" + token_path)
            pads = job.get("note_pads") or []
            if (
                job.get("note_synced")
                and job.get("note_active_pad") == "pad-2"
                and job.get("note_text") == pad_text
                and len(pads) >= 2
            ):
                break
            time.sleep(0.1)
        else:
            fail("web notepad second pad did not sync")

        if not join.stdin:
            fail("join stdin was unavailable")
        join.stdin.write("/quit\n")
        join.stdin.flush()
        output = communicate_or_fail(join, timeout=8)
        if note_text not in output:
            fail("join output did not contain synced note text:\n" + output[-2000:])
        if pad_text not in output:
            fail("join output did not contain second pad note text:\n" + output[-2000:])
    except BaseException:
        for proc in reversed(procs):
            terminate(proc)
        dump_child_output(procs)
        raise
    finally:
        for proc in reversed(procs):
            terminate(proc)

    print("web note smoke passed")


if __name__ == "__main__":
    main()
