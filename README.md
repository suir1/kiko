# kiko

`kiko` is a C++20 croc-like file transfer tool: rendezvous relay, mnemonic pairing codes, PAKE end-to-end encryption (libsodium CPace / Ristretto255 + XChaCha20-Poly1305), zstd stream compression, resumable multi-file transfers, multiplexed relay connections, IPv6-first dual-stack networking, NAT-aware adaptive TCP punching with relay fallback, and CLI or FTXUI front-ends.

## Install

Download prebuilt binaries from [Releases](https://github.com/suir1/kiko/releases), or install the latest release.
Current packages cover Linux x64, Linux ARM64, macOS ARM64, macOS x64, Windows x64, and Android / Termux ARM64.

macOS / Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.sh | sh
```

Termux ARM64:

```sh
pkg install -y curl
curl -fsSL https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.sh | sh
```

Windows PowerShell:

```powershell
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$script = irm https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.ps1
if ([string]::IsNullOrWhiteSpace($script)) { throw "failed to download kiko installer" }
iex $script
```

If `raw.githubusercontent.com` is blocked by your network, download the Windows zip from
[Releases](https://github.com/suir1/kiko/releases) and unzip `kiko.exe` into a directory on `PATH`.

The installer writes to `~/.local/bin/kiko` on macOS/Linux and `~/bin/kiko.exe` on Windows. Set
`KIKO_INSTALL_DIR` to choose another directory, or `KIKO_VERSION=v0.2.8-alpha` to install a specific release.
Use `KIKO_INSTALL_DRY_RUN=1` to print the release asset URL without downloading it.
Set `KIKO_ADD_TO_PATH=1` to add the install directory to your user PATH (`~/.zshrc`, `~/.bashrc`,
`~/.profile`, or Windows User PATH). You can also use `KIKO_ADD_TO_PATH=prompt` for an interactive
prompt when running the script from a terminal.

```sh
curl -fsSL https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.sh | KIKO_ADD_TO_PATH=1 sh
```

```powershell
$env:KIKO_ADD_TO_PATH = "1"
$script = irm https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.ps1
if ([string]::IsNullOrWhiteSpace($script)) { throw "failed to download kiko installer" }
iex $script
```

Termux source-build fallback:

```sh
pkg update
pkg install -y git clang cmake ninja pkg-config libsodium zstd
git clone https://github.com/suir1/kiko.git
cd kiko
cmake --preset system-deps
cmake --build build --target kiko
mkdir -p "$PREFIX/bin"
cp build/kiko "$PREFIX/bin/kiko"
chmod +x "$PREFIX/bin/kiko"
```

Or let the shell installer do the clone/build/install step:

```sh
curl -fsSL https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.sh | KIKO_BUILD_FROM_SOURCE=1 sh
```

On Termux, the prebuilt installer writes to `$PREFIX/bin/kiko`. `KIKO_BUILD_FROM_SOURCE=1` runs `pkg update` and installs build dependencies with `pkg install -y` by default. Set
`KIKO_INSTALL_DEPS=0` to skip that step, or `KIKO_SOURCE_REF=main` to build a branch/ref instead of the latest release tag.

macOS binaries are not signed yet. If Gatekeeper blocks the downloaded binary, open **System Settings** →
**Privacy & Security** and allow `kiko`, or remove the quarantine flag:

```sh
xattr -d com.apple.quarantine ~/.local/bin/kiko
```

Quick check:

```sh
kiko --version
kiko doctor
kiko send ./file-or-folder
kiko recv <code> --out ./downloads
```

## Features

- **Short pairing codes** – auto-generated 6-character codes (e.g. `x7k9m2`). Pass any custom code with `--code` (short alphanumeric, or croc-style `4827-stone-iris-lake-ruby` with `-` splitting room label and PAKE secret).
- **Human-sized pairing window** – send/recv/note wait up to 5 minutes by default for the other side to enter the code. Use `--pair-timeout SEC` to tune it.
- **Terminal QR code** – when sending from a TTY, a QR code for the pairing code is printed (disable with `--no-qrcode`).
- **IPv6 + IPv4 dual stack** – `AF_UNSPEC` resolve, dual-stack listeners (`::`), IPv6-first dial with IPv4 fallback. Bracketed endpoints: `[::1]:9000`.
- **Resume** – preflights the file manifest, writes to `<name>.kikopart`, renames on success, and reports the resume offset when continuing. Re-run the same send/recv pair to continue; SHA-256 verifies the whole file. **imohash** fingerprints skip files you already have.
- **Conflict policy** – receivers can `overwrite`, `skip`, or `rename` existing files with `--on-conflict`.
- **Parallel connections** – relay/direct mux uses `--connections N` (default 4), dynamic chunk scheduling, and per-stream XChaCha20-Poly1305 subkeys.
- **LAN discovery** – UDP multicast finds local relays; sender embeds a croc-style LAN relay (`--no-local` to disable). Receiver races LAN and external relays in parallel (`--local`, `--no-local`).
- **Relay health check** – JSON `ping`/`pong` on each relay connection before rendezvous registration.
- **LAN upgrade** – after the relay pipe is up, the receiver can probe sender LAN addresses before E2E PAKE.
- **`.gitignore` support** – directory sends respect `.gitignore` rules (`--no-gitignore` to disable).
- **File metadata** – executable bits are preserved, and `--symlinks preserve` can send safe relative symlinks as links.
- **Proxy** – `--proxy http://host:port` or `socks5://host:port` for relay connections.
- **NAT awareness** – LAN candidates first, then reflexive addresses; adaptive punch timing before relay fallback.
- **Connectivity planning** – STUN NAT probe (`--udp-probe`), rule-based `RoutePlan`, VPN-filtered LAN candidates, relay outbound path probing, and `~/.config/kiko/profile.json` success memory.
- **Doctor** – `kiko doctor [--udp-probe] [--json] [--ai-explain]` probes relay reachability and suggests a transfer path.
- **Shared notepad** – `kiko note host --tui` and `kiko note join CODE --tui` synchronize a temporary plaintext note over the same direct/relay and PAKE-encrypted path.
- **Local Web console** – `kiko web` opens a loopback-only browser UI for send, receive, notepad, doctor, path browsing, live progress, and cancel.
- **AI assist (BYOK, opt-in)** – OpenAI-compatible route advice and human-readable doctor explanations via env-configured API key.

## Security model

Default codes are a single short string used for both relay rendezvous and PAKE. If the code contains `-`, the part before the first `-` is a public rendezvous label and the remainder is the PAKE secret (croc-style). The relay only sees `SHA256("kiko-room:" + label)`. Peers run CPace-style PAKE over Ristretto255, mutual key confirmation, then XChaCha20-Poly1305 with HKDF-derived per-direction subkeys. The relay is a blind forwarder and cannot decrypt traffic.

## Build

Primary build uses **vcpkg manifest mode** (dependencies in [`vcpkg.json`](vcpkg.json)).

```sh
# Bootstrap local vcpkg (first time)
git clone https://github.com/microsoft/vcpkg.git .vcpkg
./.vcpkg/bootstrap-vcpkg.sh

cmake --preset local-vcpkg
cmake --build build
ctest --preset local-vcpkg
```

Presets (see [`CMakePresets.json`](CMakePresets.json)):

| Preset | Use |
|--------|-----|
| `local-vcpkg` | `.vcpkg/` checkout in the repo |
| `default` | `$VCPKG_ROOT` environment variable |
| `system-deps` | Homebrew/pkg-config fallback + FetchContent |

On macOS you may need build tools for vcpkg ports: `brew install pkg-config autoconf automake libtool`.

## Usage

Interactive TUI:

```sh
./build/kiko tui
```

Shared notepad:

```sh
# Select Notepad from the main TUI, or launch it directly:
./build/kiko tui

# First device
./build/kiko note host --tui

# Second device
./build/kiko note join <code> --tui
```

The note is temporary and kept only in memory. Edits are sent after a 250 ms pause, and the status changes to
`synced` after the peer acknowledges applying that revision. `Esc` returns to the main menu when launched from
`kiko tui`.
The Send and Receive screens keep the built-in `Paths...` browser and also expose native `File...` / `Folder...`
pickers on macOS, Windows, and graphical Linux desktops. Linux uses `zenity` or `kdialog`; when a native picker is
unavailable (including headless Linux and Termux TUI), `Paths...` remains available.

Local browser UI:

```sh
./build/kiko web
./build/kiko web --no-open --listen 127.0.0.1:0
```

`kiko web` prints a one-time tokenized local URL and only accepts loopback listeners in this version.
On desktop, the File and Folder buttons open the host operating system picker. In Termux, File opens the
Android browser document picker for one file and stages it over loopback until the send finishes. Paths keeps
the embedded filesystem browser available for folders and as a fallback; staged files are removed automatically.
`kiko web` uses `termux-open-url` to open the tokenized loopback URL in the Android browser.
It also holds a Termux wake lock until the Web process exits so Android is less likely to suspend the loopback
server after switching to the browser. Use `--no-wake-lock` if you manage the wake lock yourself. Vendor battery
savers can still kill background apps; set Termux battery usage to unrestricted if the page remains unreachable.
The Web Notepad tab uses the same pairing code and encrypted direct/relay path as `kiko note`; it is plaintext,
temporary, and kept only in memory.

Run a relay (LAN-announced via multicast):

```sh
./build/kiko relay --listen [::]:9000
./build/kiko relay --listen [::]:9000 --pass my-secret --room-ttl 10800
```

Run a public/server relay without the full CLI/TUI frontend:

```sh
./build/kiko-relayd --listen 0.0.0.0:9000
./build/kiko-relayd --listen 0.0.0.0:9000 --pass my-secret --room-ttl 10800
```

Clients use `--relay-pass` or `KIKO_RELAY_PASS` when the relay requires a password.

`kiko` uses `106.53.170.243:9000` as the built-in public relay when `KIKO_RELAY` is unset.
Saved settings in `~/.config/kiko/config.json` (override with `KIKO_CONFIG_PATH`) are used when env vars are unset.
`kiko tui` updates that file automatically; CLI uses `--remember` on `send` / `recv` after a successful transfer.
Override it for private/self-hosted relays:

```sh
export KIKO_RELAY=relay.example.com:9000
export KIKO_RELAY_PASS=my-secret
```

Build with a compile-time default relay (when `KIKO_RELAY` is unset):

```sh
cmake -DKIKO_DEFAULT_RELAY=relay.example.com:9000 --preset local-vcpkg
```

Send:

```sh
./build/kiko send ./file.bin
./build/kiko send ./my-folder --relay [::1]:9000 --connections 8
./build/kiko send ./repo --proxy socks5://127.0.0.1:1080
./build/kiko send ./repo --symlinks preserve
./build/kiko send ./file.bin --debug-route
```

Receive:

```sh
./build/kiko recv <code> --out ./downloads
./build/kiko recv <code> --out ./downloads --on-conflict rename
```

Network diagnostics:

```sh
./build/kiko doctor
./build/kiko doctor --udp-probe --json
./build/kiko doctor --ai-explain   # requires OPENAI_API_KEY or KIKO_AI_API_KEY
```

`doctor` reports local LAN IPs, VPN/TUN IPs, the route/interface used for the relay target, and STUN mapped
addresses when `--udp-probe` is enabled. This helps spot cases where a relay IP is being captured by a TUN
interface instead of going out through the physical LAN/Wi-Fi route.

When VPN/TUN interfaces are detected and no proxy or manual bind is set, `send`, `recv`, and `doctor` probe both
the default relay path and a likely physical interface path, then use the reachable path with lower relay RTT.
The `doctor --json` output includes `outbound_path`, `outbound_reason`, and `outbound_probes` for this decision.
It also includes `relay_reachable`, `recommendation`, `direct_probe`, and `route_result_hint`; the hint separates
the rendezvous relay from whether file data is expected to require relay forwarding.

To test a physical-interface route while VPN/TUN is active:

```sh
./build/kiko doctor --avoid-vpn
./build/kiko doctor --bind-interface en0
```

`--avoid-vpn` picks a likely physical interface automatically. `--bind-interface <name>` forces outbound TCP
sockets onto a named interface where the platform supports it (macOS uses `IP_BOUND_IF` / `IPV6_BOUND_IF`;
Linux tries `SO_BINDTODEVICE`, which may require privileges). These options apply to relay probing, relay
rendezvous, relay mux connections, and active direct TCP dials. If the scoped physical route exists but relay
ping still fails, the physical network or relay port may be blocked; use a VPN DIRECT rule or a reachable relay
port.

Use `send --debug-route` or `recv --debug-route` to print a short doctor-style route summary before transfer,
including relay reachability, outbound interface choice, direct probe budget, route hint, and recommendation.

Optional AI configuration (BYOK):

```sh
export OPENAI_API_KEY=sk-...
export KIKO_AI_BASE_URL=https://api.openai.com/v1   # or http://localhost:11434/v1 for local
export KIKO_AI_MODEL=gpt-4o-mini
```

Flags: `--relay-pass`, `--pair-timeout`, `--remember`, `--auto-connections`, `--debug-route`, `--no-direct`, `--no-lan`, `--no-local`, `--local`, `--ip`, `--udp-probe`, `--ai-route`, `--ai-route-plan-only`, `--no-gitignore`, `--symlinks`, `--on-conflict`, `--no-qrcode`, `--proxy`, `--bind-interface`, `--avoid-vpn`, `--tui`.

`--ip` overrides the relay target (port from `--relay`) and the addresses advertised to the peer for direct/punch paths. When set, LAN relay discovery is skipped.

## Tests

```sh
ctest --preset local-vcpkg --output-on-failure
```

Memory-safety checks:

```sh
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

Route smoke checks:

```sh
# Relay data path: default, deterministic, same as CI smoke.
python3 scripts/relay_transfer_smoke.py ./build/kiko

# Same-machine direct path: useful before testing two real devices.
python3 scripts/relay_transfer_smoke.py ./build/kiko \
  --allow-direct --allow-lan --allow-local \
  --expect-data-path direct

# Public relay / IPv6 direct expectation: use on hosts that both advertise global IPv6.
python3 scripts/relay_transfer_smoke.py ./build/kiko \
  --relay 106.53.170.243:9000 \
  --allow-direct --allow-lan --allow-local \
  --expect-data-path direct \
  --expect-candidate ipv6_global \
  --expect-family ipv6 \
  --expect-scope global
```

The complete automated and VPS manual regression matrix is in
[`docs/test-scenarios.md`](docs/test-scenarios.md).

Docker network labs:

```sh
scripts/netlab_matrix.sh
scripts/netlab_double_nat.sh
```

`netlab_matrix.sh` currently verifies fake-VPN outbound physical fallback in `doctor`, manual
`--avoid-vpn`/`--bind-interface` overrides, fake-VPN relay transfer over the physical outbound path,
sender-only and receiver-only fake-VPN relay transfers, delayed/limited relay transfer with `tc netem`,
same-LAN direct TCP, same-LAN isolated relay fallback, one-side-NAT direct TCP, and Docker double-NAT
fast relay fallback after direct probing. The first Docker run builds the `kiko-netlab:local` image;
later runs reuse the cache.

The Docker labs are also available as optional CTest targets:

```sh
cmake -S . -B build -DKIKO_ENABLE_NETLAB_TESTS=ON
ctest --test-dir build -L netlab --output-on-failure
```

`KIKO_ENABLE_NETLAB_TESTS` defaults to `OFF` so normal `ctest` runs do not require Docker or privileged networking.
The `Docker Netlab` GitHub workflow runs the same matrix manually and once per week.

- `handshake` – PAKE session key agreement and rejection of wrong codes
- `transfer` – multi-file directory round-trip, manifest preflight, resume from partial
- `progress` – `ProgressReporter` event sequence
- `mux` – parallel relay connections and resume
- `adaptive` – NAT classification and punch planning
- `connectivity` – RoutePlan rule generation
- `ai` – AI route plan validation and merge

## CI

`CI Fast` runs automatically on pushes and pull requests with Ubuntu x64, macOS ARM, and Windows. Use it for normal development feedback.

`CI Full` is manual (`workflow_dispatch`) and runs the full platform matrix: Ubuntu x64/ARM, macOS ARM/Intel, and Windows. It also keeps the optional public relay doctor + send/recv smoke test when `KIKO_RELAY_SMOKE` is set. Run it before releases or large networking/relay changes.

## Releases

Tags matching `v*` trigger the release workflow. It builds and tests Linux, macOS, and Windows packages, then
uploads the packaged binaries to the matching GitHub Release.

```sh
git tag v0.2.2-alpha
git push origin v0.2.2-alpha
```

## Stack

| Layer | Choice |
|-------|--------|
| Crypto | libsodium (Ristretto255 PAKE, XChaCha20-Poly1305, SHA-256/HKDF) |
| Network | Standalone Asio + synchronous socket I/O |
| Compression | zstd |
| CLI | CLI11 + nlohmann-json control messages |
| UI | FTXUI (`tui` / `--tui`) and embedded loopback Web UI (`web`) |
| QR | Nayuki qrcodegen (vcpkg) |
