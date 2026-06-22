#!/usr/bin/env python3
"""End-to-end smoke test: relay + CLI send/recv + route/content check."""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_KIKO = ROOT / "build" / "kiko"


@dataclass
class SmokeOptions:
    kiko: Path
    relay: str | None = None
    allow_direct: bool = False
    allow_lan: bool = False
    allow_local: bool = False
    expect_data_path: str = "relay"
    expect_candidate: str | None = None
    expect_family: str | None = None
    expect_scope: str | None = None


def fail(message: str, *logs: tuple[str, str]) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    for title, text in logs:
        if text:
            print(f"---- {title} ----", file=sys.stderr)
            print(text[-6000:], file=sys.stderr)
    sys.exit(1)


def usage() -> str:
    return (
        "usage: relay_transfer_smoke.py [/path/to/kiko] [--relay HOST:PORT]\n"
        "       [--allow-direct] [--allow-lan] [--allow-local]\n"
        "       [--expect-data-path relay|direct|any]\n"
        "       [--expect-candidate KIND] [--expect-family ipv4|ipv6|unknown]\n"
        "       [--expect-scope global|private|unique_local|link_local|loopback|unknown]\n"
        "\n"
        "Default mode starts a local relay, disables direct/LAN/local paths, and expects relay data.\n"
    )


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


def parse_args(argv: list[str]) -> SmokeOptions:
    kiko = Path(os.environ.get("KIKO_BIN", DEFAULT_KIKO))
    relay: str | None = None
    allow_direct = False
    allow_lan = False
    allow_local = False
    expect_data_path = "relay"
    expect_candidate: str | None = None
    expect_family: str | None = None
    expect_scope: str | None = None
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg in {"-h", "--help"}:
            print(usage(), end="")
            sys.exit(0)
        if arg == "--relay":
            if i + 1 >= len(argv):
                fail("--relay requires HOST:PORT")
            relay = argv[i + 1]
            i += 2
        elif arg == "--allow-direct":
            allow_direct = True
            i += 1
        elif arg == "--allow-lan":
            allow_lan = True
            i += 1
        elif arg == "--allow-local":
            allow_local = True
            i += 1
        elif arg == "--expect-data-path":
            if i + 1 >= len(argv):
                fail("--expect-data-path requires relay, direct, or any")
            expect_data_path = argv[i + 1]
            if expect_data_path not in {"relay", "direct", "any"}:
                fail("--expect-data-path must be relay, direct, or any")
            i += 2
        elif arg == "--expect-candidate":
            if i + 1 >= len(argv):
                fail("--expect-candidate requires a direct candidate kind")
            expect_candidate = argv[i + 1]
            i += 2
        elif arg == "--expect-family":
            if i + 1 >= len(argv):
                fail("--expect-family requires ipv4, ipv6, or unknown")
            expect_family = argv[i + 1]
            i += 2
        elif arg == "--expect-scope":
            if i + 1 >= len(argv):
                fail("--expect-scope requires a scope name")
            expect_scope = argv[i + 1]
            i += 2
        elif arg.startswith("--"):
            fail(f"unknown option: {arg}")
        else:
            kiko = Path(arg)
            i += 1
    return SmokeOptions(
        kiko=kiko,
        relay=relay,
        allow_direct=allow_direct,
        allow_lan=allow_lan,
        allow_local=allow_local,
        expect_data_path=expect_data_path,
        expect_candidate=expect_candidate,
        expect_family=expect_family,
        expect_scope=expect_scope,
    )


ROUTE_PAIR = re.compile(r"([a-zA-Z_]+)=([^ ]+)")


def route_summaries(text: str) -> list[dict[str, str]]:
    summaries: list[dict[str, str]] = []
    for line in text.splitlines():
        if not line.startswith("route summary:"):
            continue
        summaries.append({key: value for key, value in ROUTE_PAIR.findall(line)})
    return summaries


def candidate_kind(value: str | None) -> str:
    if not value:
        return ""
    return value.split("#", 1)[0]


def matching_route(summary: dict[str, str], options: SmokeOptions) -> bool:
    if options.expect_data_path != "any" and summary.get("data") != options.expect_data_path:
        return False
    if options.expect_candidate and candidate_kind(summary.get("candidate")) != options.expect_candidate:
        return False
    if options.expect_family and summary.get("family") != options.expect_family:
        return False
    if options.expect_scope and summary.get("scope") != options.expect_scope:
        return False
    return True


def assert_route_expectations(text: str, options: SmokeOptions) -> None:
    summaries = route_summaries(text)
    if not summaries:
        fail("transfer logs did not include route summary", ("transfer logs", text))
    if any(matching_route(summary, options) for summary in summaries):
        return

    expectation = f"data={options.expect_data_path}"
    if options.expect_candidate:
        expectation += f" candidate={options.expect_candidate}"
    if options.expect_family:
        expectation += f" family={options.expect_family}"
    if options.expect_scope:
        expectation += f" scope={options.expect_scope}"
    rendered = "\n".join(str(summary) for summary in summaries)
    fail(f"route summary did not match expected {expectation}", ("route summaries", rendered), ("transfer logs", text))


def append_connectivity_flags(command: list[str], options: SmokeOptions) -> None:
    if not options.allow_direct:
        command.append("--no-direct")
    if not options.allow_lan:
        command.append("--no-lan")
    if not options.allow_local:
        command.append("--no-local")


def run_pair(options: SmokeOptions, work: Path, relay: str) -> None:
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
        str(options.kiko),
        "recv",
        code,
        "--relay",
        relay,
        "--out",
        str(recv_dir),
    ]
    append_connectivity_flags(recv_cmd, options)
    send_cmd = [
        str(options.kiko),
        "send",
        str(source_file),
        "--relay",
        relay,
        "--code",
        code,
        "--no-qrcode",
        "--connections",
        "2",
    ]
    append_connectivity_flags(send_cmd, options)

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
    assert_route_expectations(combined, options)
    print("relay transfer smoke passed")


def main() -> None:
    options = parse_args(sys.argv)
    if not options.kiko.is_file():
        fail(f"kiko binary not found: {options.kiko}")

    work = Path(tempfile.mkdtemp(prefix="kiko-relay-transfer-smoke-"))
    relay_proc: subprocess.Popen[str] | None = None
    try:
        relay = options.relay
        if relay is None:
            port = free_tcp_port()
            relay = f"127.0.0.1:{port}"
            relay_proc = subprocess.Popen(
                [str(options.kiko), "relay", "--listen", relay],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if not wait_for_port(port):
                relay_log = terminate(relay_proc)
                fail(f"relay did not listen on {relay}", ("relay", relay_log))
        run_pair(options, work, relay)
    finally:
        terminate(relay_proc)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
