#!/usr/bin/env python3
"""Optional browser smoke for kiko web.

This test exercises the embedded UI with Playwright when it is available. It
returns 77 when Playwright or a browser runtime is missing; CTest treats that as
a skip.
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path


SKIP = 77


def skip(message: str) -> None:
    print(f"SKIP: {message}")
    raise SystemExit(SKIP)


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


try:
    from playwright.sync_api import Error as PlaywrightError
    from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
    from playwright.sync_api import sync_playwright
except Exception as exc:  # pragma: no cover - environment dependent
    skip(f"python playwright is not installed: {exc}")


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


def assert_visible(page, selector: str, message: str) -> None:
    if not page.locator(selector).is_visible():
        fail(message)


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: web_browser_smoke.py /path/to/kiko")
    kiko = Path(sys.argv[1])
    proc = subprocess.Popen(
        [str(kiko), "web", "--no-open", "--listen", "127.0.0.1:0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        url = read_url(proc)
        page_errors: list[str] = []
        with sync_playwright() as p:
            try:
                browser = p.chromium.launch(headless=True)
            except PlaywrightError as exc:
                skip(f"playwright browser is not installed: {exc}")
            with browser:
                page = browser.new_page()
                page.on("pageerror", lambda error: page_errors.append(str(error)))
                page.goto(url, wait_until="domcontentloaded")
                page.wait_for_selector("text=kiko web", timeout=5000)

                assert_visible(page, "#tab-send", "send tab should be visible")

                page.click("#send-start")
                page.wait_for_selector("text=Choose a file or folder before starting send.", timeout=3000)

                page.click("#tab-recv")
                page.click("#recv-start")
                page.wait_for_selector("text=Enter the pairing code shown on the other device.", timeout=3000)

                page.click("#tab-note")
                page.select_option("#note-role", "join")
                page.click("#note-start")
                page.wait_for_selector("text=Choose Join only after entering the host notepad code.", timeout=3000)

                page.click("#tab-send")
                page.click("#panel-send button:has-text('Browse')")
                page.wait_for_function(
                    "() => !document.getElementById('browser').classList.contains('hidden')",
                    timeout=5000,
                )
                page.fill("#browser-filter", "definitely-no-such-file")
                page.click("button:has-text('Clear')")
                if page.locator("#browser-filter").input_value() != "":
                    fail("browser clear should empty the filter")
                page.click("button:has-text('Close')")
                page.wait_for_function(
                    "() => document.getElementById('browser').classList.contains('hidden')",
                    timeout=3000,
                )

                if page_errors:
                    fail("page error: " + page_errors[0])
    except PlaywrightTimeoutError as exc:
        fail(f"browser smoke timed out: {exc}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    print("web browser smoke passed")


if __name__ == "__main__":
    main()
