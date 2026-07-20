#!/usr/bin/env python3
"""Browser smoke for the embedded kiko web console."""

from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


SKIP = 77
REQUIRE_BROWSER = os.environ.get("KIKO_REQUIRE_BROWSER_SMOKE") == "1"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def skip(message: str) -> None:
    if REQUIRE_BROWSER:
        fail(f"required browser smoke unavailable: {message}")
    print(f"SKIP: {message}")
    raise SystemExit(SKIP)


try:
    from playwright.sync_api import Error as PlaywrightError
    from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
    from playwright.sync_api import sync_playwright
except Exception as exc:  # pragma: no cover - environment dependent
    skip(f"python playwright is not installed: {exc}")


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


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


def terminate(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def stop_interactive(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    if proc.stdin:
        try:
            proc.stdin.write("/quit\n")
            proc.stdin.flush()
            proc.communicate(timeout=8)
            return
        except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
            pass
    terminate(proc)


def assert_visible(page, selector: str, message: str) -> None:
    if not page.locator(selector).is_visible():
        fail(message)


def wait_for_note_state(page, timeout: float = 12) -> None:
    page.wait_for_function(
        "() => /connected|synced/.test(document.getElementById('note-meta').textContent)",
        timeout=timeout * 1000,
    )


def wait_for_idle(page, timeout: float = 8) -> None:
    page.wait_for_function(
        "() => document.getElementById('cancel').disabled && !document.getElementById('note-start').disabled",
        timeout=timeout * 1000,
    )


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: web_browser_smoke.py /path/to/kiko /path/to/kiko-relayd")

    kiko = Path(sys.argv[1])
    relayd = Path(sys.argv[2])
    relay_addr = f"127.0.0.1:{free_tcp_port()}"
    relay = subprocess.Popen(
        [str(relayd), "--listen", relay_addr],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    web: subprocess.Popen[str] | None = None
    children: list[subprocess.Popen[str]] = []
    stage = "startup"

    try:
        time.sleep(0.2)
        if relay.poll() is not None:
            fail("kiko-relayd exited early")

        web = subprocess.Popen(
            [
                str(kiko),
                "web",
                "--no-open",
                "--listen",
                "127.0.0.1:0",
                "--relay",
                relay_addr,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        url = read_url(web)
        start_bodies: list[dict] = []
        qr_bodies: list[dict] = []
        page_errors: list[str] = []

        with sync_playwright() as p:
            try:
                browser = p.chromium.launch(headless=True)
            except PlaywrightError as exc:
                skip(f"playwright browser is not installed: {exc}")
            with browser:
                context = browser.new_context(permissions=["clipboard-read", "clipboard-write"])
                try:
                    page = context.new_page()
                    page.on("pageerror", lambda error: page_errors.append(str(error)))

                    def capture_request(request) -> None:
                        if request.method != "POST":
                            return
                        if "/api/note/start" in request.url:
                            start_bodies.append(json.loads(request.post_data or "{}"))
                        elif "/api/qr" in request.url:
                            qr_bodies.append(json.loads(request.post_data or "{}"))

                    page.on("request", capture_request)
                    stage = "page load"
                    page.goto(url, wait_until="domcontentloaded")
                    page.wait_for_selector("text=kiko web", timeout=5000)
                    page.wait_for_function(
                        "() => document.getElementById('server').textContent !== 'local console'",
                        timeout=5000,
                    )

                    assert_visible(page, "#tab-send", "send tab should be visible")

                    page.fill("#send-path", "")
                    if page.locator("#send-path").input_value() != "":
                        fail("send path should be empty before validation")
                    page.click("#send-start")
                    page.wait_for_selector("text=Choose a file or folder before starting send.", timeout=3000)

                    page.click("#tab-recv")
                    page.click("#recv-start")
                    page.wait_for_selector("text=Enter the pairing code shown on the other device.", timeout=3000)

                    page.click("#tab-note")
                    assert_visible(page, "#note-start", "notepad start button should be visible")
                    assert_visible(page, "#note-custom-host", "custom-code host option should be visible")
                    if page.locator("#note-custom-host").is_checked():
                        fail("custom-code host should be disabled by default")
                    page.locator("#panel-note details summary").click()
                    page.check("#note-no-direct")
                    page.uncheck("#note-lan")
                    page.uncheck("#note-local-relay")

                    # Empty code selects Host and generates a code for the CLI peer.
                    stage = "Host code generation"
                    page.click("#note-start")
                    page.wait_for_function(
                        "() => document.getElementById('note-code').value.trim() !== ''",
                        timeout=8000,
                    )
                    if not start_bodies or start_bodies[-1].get("role") != "host":
                        fail(f"empty-code start did not select Host: {start_bodies!r}")
                    host_code = page.locator("#note-code").input_value().strip()

                    join = subprocess.Popen(
                        [
                            str(kiko),
                            "note",
                            "join",
                            host_code,
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
                    children.append(join)
                    stage = "Host peer connection"
                    wait_for_note_state(page)

                    note_text = "browser text ✓"
                    stage = "main note synchronization"
                    page.fill("#note-text", note_text)
                    page.wait_for_function(
                        "text => document.getElementById('note-text').value === text",
                        arg=note_text,
                        timeout=3000,
                    )
                    page.wait_for_function(
                        "() => document.getElementById('note-meta').textContent.includes('synced')",
                        timeout=8000,
                    )

                    page.click("#note-copy-code")
                    page.wait_for_function(
                        "() => document.getElementById('error-hint').textContent === 'Code copied.'",
                        timeout=3000,
                    )
                    if page.evaluate("async () => await navigator.clipboard.readText()") != host_code:
                        fail("Copy code did not write the pairing code")

                    page.click("#note-copy-note")
                    page.wait_for_function(
                        "() => document.getElementById('error-hint').textContent === 'Note copied.'",
                        timeout=3000,
                    )
                    if page.evaluate("async () => await navigator.clipboard.readText()") != note_text:
                        fail("Copy note did not write the note text")

                    page.click("#note-qr")
                    page.wait_for_function(
                        "() => !document.getElementById('qr-modal').classList.contains('hidden')",
                        timeout=5000,
                    )
                    if page.locator("#qr-content svg").count() != 1:
                        fail("QR modal did not render an SVG")
                    if not qr_bodies or qr_bodies[-1].get("text") != note_text:
                        fail(f"QR request did not contain the note text directly: {qr_bodies!r}")
                    if "server URL" not in page.locator("#qr-meta").text_content():
                        fail("QR modal did not explain that it contains direct text")
                    page.locator("#qr-modal button").click()

                    stage = "Pad 2 creation"
                    page.click("#note-new-pad")
                    page.get_by_role("button", name="Note 2", exact=True).wait_for(timeout=5000)
                    pad_text = "browser pad two"
                    page.fill("#note-text", pad_text)
                    page.wait_for_function(
                        "text => document.getElementById('note-text').value === text",
                        arg=pad_text,
                        timeout=3000,
                    )
                    page.get_by_role("button", name="Note 1", exact=True).click()
                    stage = "Pad 1 restore"
                    page.wait_for_function(
                        "text => document.getElementById('note-text').value === text",
                        arg=note_text,
                        timeout=5000,
                    )
                    page.get_by_role("button", name="Note 2", exact=True).click()
                    stage = "Pad 2 restore"
                    page.wait_for_function(
                        "text => document.getElementById('note-text').value === text",
                        arg=pad_text,
                        timeout=5000,
                    )
                    page.click("#note-clear")
                    page.wait_for_function(
                        "() => document.getElementById('note-text').value === ''",
                        timeout=3000,
                    )
                    page.get_by_role("button", name="Note 1", exact=True).click()
                    stage = "Pad clear isolation"
                    page.wait_for_function(
                        "text => document.getElementById('note-text').value === text",
                        arg=note_text,
                        timeout=5000,
                    )

                    stop_interactive(join)
                    stage = "Host session close"
                    if page.locator("#note-start").is_disabled():
                        page.click("#cancel")
                    wait_for_idle(page)
                    children.remove(join)

                    # A non-empty code selects Join. The CLI host supplies that code.
                    join_code = "browserjoin"
                    cli_host = subprocess.Popen(
                        [
                            str(kiko),
                            "note",
                            "host",
                            "--code",
                            join_code,
                            "--relay",
                            relay_addr,
                            "--no-direct",
                            "--no-lan",
                            "--no-local",
                            "--no-qrcode",
                        ],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                    )
                    children.append(cli_host)
                    page.fill("#note-code", join_code)
                    stage = "non-empty-code Join start"
                    page.click("#note-start")
                    page.wait_for_function(
                        "() => document.getElementById('note-meta').textContent.includes('connecting') || document.getElementById('note-meta').textContent.includes('connected')",
                        timeout=5000,
                    )
                    page.wait_for_function(
                        "code => document.getElementById('note-code').value === code",
                        arg=join_code,
                        timeout=3000,
                    )
                    if not start_bodies or start_bodies[-1].get("role") != "join":
                        fail(f"non-empty-code start did not select Join: {start_bodies!r}")
                    stage = "Join peer connection"
                    wait_for_note_state(page)
                    stop_interactive(cli_host)
                    children.remove(cli_host)

                    if page_errors:
                        fail("page error: " + page_errors[0])
                finally:
                    context.close()
    except PlaywrightTimeoutError as exc:
        fail(f"browser smoke timed out during {stage}: {exc}")
    finally:
        for child in reversed(children):
            stop_interactive(child)
        if web is not None:
            terminate(web)
        terminate(relay)
    print("web browser smoke passed")


if __name__ == "__main__":
    main()
