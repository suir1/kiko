# Test Scenarios

This document separates deterministic automated checks from the manual checks
that require two real devices or the public VPS relay.

## Local Automated Gate

Run the following before pushing a networking or Notepad change:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

The gate covers:

- Relay ping/pong, advertised version, password rejection, rendezvous, direct
  confirmation, relay fallback, stale waiting peers, and punch-port mapping.
- Note frame encoding, stale revision rejection, clear, size limits, multiple
  pads, pad selection, per-pad acknowledgements, and synchronization state.
- Web token protection, Web Notepad pairing through a relay-only path,
  Unicode/multiline text, pad creation, pad switching, clear, and pad-content
  isolation.
- Host and Join pairing timeouts, including the no-peer case for both roles.
- Relay file transfer, TUI rendering, browser API smoke, and installer smoke.

`web_browser_smoke` may skip on developer machines without Playwright. Ubuntu
CI installs Playwright Chromium and sets `KIKO_REQUIRE_BROWSER_SMOKE=1`, so a
missing browser runtime fails CI instead of being reported as a skip.

To run the same strict browser gate locally:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install playwright==1.52.0
.venv/bin/python -m playwright install chromium
cmake --preset local-vcpkg -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
KIKO_REQUIRE_BROWSER_SMOKE=1 ctest --test-dir build -R web_browser_smoke --output-on-failure
```

The browser scenario verifies empty-code Host, non-empty-code Join, Unicode
note editing, copy of the code and note, direct-text QR requests, multiple-pad
switching, and clearing one pad without changing another.

## VPS Manual Gate

Use the released `v0.2.5-alpha` binaries on two devices.

### Relay health

```sh
kiko doctor --relay 106.53.170.243:9000 --json
```

Expected: `relay_reachable=true`, `pong_ok=true`, and Relay `version` matching
the deployed `kiko-relayd` version.

### Relay-only Notepad

Run the same-host integration preflight through the public Relay before the
two-device UI check:

```sh
python3 scripts/web_note_smoke.py ./build/kiko --relay 106.53.170.243:9000
```

This covers Web Host, CLI Join, synchronization, Unicode text, multiple pads,
pad isolation, and peer shutdown. It does not replace the two-device TUI check.

On device A:

```sh
kiko note host --relay 106.53.170.243:9000 --no-direct --no-lan --no-local --tui
```

On device B, enter the displayed code:

```sh
kiko note join CODE --relay 106.53.170.243:9000 --no-direct --no-lan --no-local --tui
```

Verify both sides reach `connected`/`synced`, then test:

1. ASCII, Chinese, emoji, tabs, multiline text, and an empty document.
2. Rapid edits followed by a pause; the final text must converge on both sides.
3. Create Pad 2, edit it, switch back to Pad 1, and confirm Pad 1 is unchanged.
4. Clear Pad 2, switch to Pad 1, and confirm clearing Pad 2 did not clear Pad 1.
5. Close one side; the other side must leave the session instead of hanging forever.

### Web Host and CLI Join

On device A:

```sh
kiko web --relay 106.53.170.243:9000
```

Start Notepad with an empty code, copy the generated code, and join from device
B with the CLI. Repeat with `--no-direct` enabled in the Web advanced options.
Verify that Web Pad switching and QR display do not change the pairing code or
send a server URL as QR content.

### Failure paths

- Join with an underscore or other invalid character: show a clear validation
  error and do not open a session.
- Start Host with `--pair-timeout 5` and no peer: exit near the configured
  timeout with a rendezvous failure, not an indefinite wait.
- Use different Relay addresses on the two sides: fail with a relay/peer hint.
- Stop the Relay while both peers are waiting: both clients must fail promptly.
- Enter a previously used code after the peer has closed: fail and require a
  fresh Host session.

## Release Gate

Before creating a new alpha tag:

1. Run the local automated gate.
2. Run CI Full for all supported platforms.
3. Run the release workflow and verify Linux ABI checks.
4. Deploy only the official Linux x64 asset to the VPS.
5. Repeat the Relay health and Relay-only Notepad checks above.
6. Confirm the service is active with zero unexpected restarts.
