# kiko

`kiko` is a C++20 croc-like file transfer tool: rendezvous relay, mnemonic pairing codes, PAKE end-to-end encryption (libsodium CPace / Ristretto255 + XChaCha20-Poly1305), zstd stream compression, resumable multi-file transfers, multiplexed relay connections, IPv6-first dual-stack networking, NAT-aware adaptive TCP punching with relay fallback, and CLI or FTXUI front-ends.

## Features

- **Short pairing codes** – auto-generated 6-character codes (e.g. `x7k9m2`). Pass any custom code with `--code` (short alphanumeric, or croc-style `4827-stone-iris-lake-ruby` with `-` splitting room label and PAKE secret).
- **Terminal QR code** – when sending from a TTY, a QR code for the pairing code is printed (disable with `--no-qrcode`).
- **IPv6 + IPv4 dual stack** – `AF_UNSPEC` resolve, dual-stack listeners (`::`), IPv6-first dial with IPv4 fallback. Bracketed endpoints: `[::1]:9000`.
- **Resume** – writes to `<name>.kikopart`, renames on success. Re-run the same send/recv pair to continue; SHA-256 verifies the whole file. **imohash** fingerprints skip files you already have.
- **Parallel connections** – relay path uses `--connections N` (default 4) with per-stream XChaCha20-Poly1305 subkeys.
- **LAN discovery** – UDP multicast finds local relays; sender embeds a croc-style LAN relay (`--no-local` to disable). Receiver races LAN and external relays in parallel (`--local`, `--no-local`).
- **Relay health check** – JSON `ping`/`pong` on each relay connection before rendezvous registration.
- **LAN upgrade** – after the relay pipe is up, the receiver can probe sender LAN addresses before E2E PAKE.
- **`.gitignore` support** – directory sends respect `.gitignore` rules (`--no-gitignore` to disable).
- **Proxy** – `--proxy http://host:port` or `socks5://host:port` for relay connections.
- **NAT awareness** – LAN candidates first, then reflexive addresses; adaptive punch timing before relay fallback.
- **Connectivity planning** – STUN NAT probe (`--udp-probe`), rule-based `RoutePlan`, VPN-filtered LAN candidates, relay outbound path probing, and `~/.config/kiko/profile.json` success memory.
- **Doctor** – `kiko doctor [--udp-probe] [--json] [--ai-explain]` probes relay reachability and suggests a transfer path.
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

Run a relay (LAN-announced via multicast):

```sh
./build/kiko relay --listen [::]:9000
./build/kiko relay --listen [::]:9000 --pass my-secret --room-ttl 10800
```

Clients use `--relay-pass` or `KIKO_RELAY_PASS` when the relay requires a password.

`kiko` uses `106.53.170.243:9000` as the built-in public relay when `KIKO_RELAY` is unset.
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
```

Receive:

```sh
./build/kiko recv <code> --out ./downloads
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

Optional AI configuration (BYOK):

```sh
export OPENAI_API_KEY=sk-...
export KIKO_AI_BASE_URL=https://api.openai.com/v1   # or http://localhost:11434/v1 for local
export KIKO_AI_MODEL=gpt-4o-mini
```

Flags: `--relay-pass`, `--auto-connections`, `--no-direct`, `--no-lan`, `--no-local`, `--local`, `--ip`, `--udp-probe`, `--ai-route`, `--ai-route-plan-only`, `--no-gitignore`, `--no-qrcode`, `--proxy`, `--bind-interface`, `--avoid-vpn`, `--tui`.

`--ip` overrides the relay target (port from `--relay`) and the addresses advertised to the peer for direct/punch paths. When set, LAN relay discovery is skipped.

## Tests

```sh
ctest --preset local-vcpkg --output-on-failure
```

Docker network labs:

```sh
scripts/netlab_matrix.sh
scripts/netlab_double_nat.sh
```

`netlab_matrix.sh` currently verifies fake-VPN outbound physical fallback in `doctor`, manual
`--avoid-vpn`/`--bind-interface` overrides, fake-VPN relay transfer over the physical outbound path,
sender-only and receiver-only fake-VPN relay transfers, same-LAN direct TCP, same-LAN isolated relay
fallback, one-side-NAT direct TCP, and double-NAT relay fallback. The first Docker run builds the
`kiko-netlab:local` image; later runs reuse the cache.

The Docker labs are also available as optional CTest targets:

```sh
cmake -S . -B build -DKIKO_ENABLE_NETLAB_TESTS=ON
ctest --test-dir build -L netlab --output-on-failure
```

`KIKO_ENABLE_NETLAB_TESTS` defaults to `OFF` so normal `ctest` runs do not require Docker or privileged networking.

- `handshake` – PAKE session key agreement and rejection of wrong codes
- `transfer` – multi-file directory round-trip, resume from partial
- `progress` – `ProgressReporter` event sequence
- `mux` – parallel relay connections and resume
- `adaptive` – NAT classification and punch planning
- `connectivity` – RoutePlan rule generation
- `ai` – AI route plan validation and merge

## Stack

| Layer | Choice |
|-------|--------|
| Crypto | libsodium (Ristretto255 PAKE, XChaCha20-Poly1305, SHA-256/HKDF) |
| Network | Standalone Asio + C++20 coroutines (relay) / sync I/O (transfer) |
| Compression | zstd |
| CLI | CLI11 + nlohmann-json control messages |
| UI | FTXUI (optional `--tui`) |
| QR | Nayuki qrcodegen (vcpkg) |
