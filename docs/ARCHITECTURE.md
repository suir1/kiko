# kiko architecture

`kiko` is currently a single CMake project with one core static library, one CLI/TUI executable,
and one lightweight `kiko-relayd` executable for server relay deployment.
The structure is still workable, but the source tree is moving from a small flat layout toward a
project that needs explicit module boundaries.

## Current modules

- `core`: protocol framing, crypto, compression, metadata, file manifests, progress reporting.
- `transfer`: send/recv orchestration, transfer streams, mux transfer, resume, heuristics.
- `network`: sockets, relay sessions, direct sessions, LAN discovery, LAN upgrade, UDP punch, probes, proxy.
- `relay`: rendezvous relay server and relay control path.
- `connectivity`: route planning, outbound policy, NAT/adaptive decisions, rendezvous wait/race,
  post-rendezvous route selection, doctor diagnostics.
- `config`: runtime profile and user config.
- `ui`: FTXUI menu/progress screens, path browser, clipboard, advanced network options.
- `ai`: BYOK route advice and doctor explanations.
- `release`: GitHub Actions release packaging and installer smoke tests.

The code still lives mostly under flat `src/` and `tests/` directories. That is acceptable for now,
but new work should preserve these conceptual modules even before directories are physically moved.

## Friction points

### Transfer file size

`src/transfer_stream.cpp` remains the largest transfer file. It now delegates receive-plan
preflight to `src/receive_plan.*`, manifest JSON encode/decode to `src/transfer_manifest.*`,
and resume negotiation / `.kikopart` helpers to `src/transfer_resume.*`,
but still carries several responsibilities:

- tagged transfer frame helpers
- receive path conflict/finalize helpers
- single-stream send/receive loops

This is now the most important architecture cleanup target. New transfer behavior should avoid adding
more logic directly to `src/transfer_stream.cpp`; prefer extracting path/finalize helpers before
changing the stream coordinator.

### Flat source tree

The flat `src/` directory makes it harder to see ownership. A future directory move should be
mechanical and preserve names, for example:

```text
src/
  app/
  core/
  transfer/
  network/
  relay/
  connectivity/
  config/
  ui/
  ai/
```

Do this only after current TUI/config work settles, because a large file move during active feature
work creates unnecessary merge friction.

### CMake growth

`CMakeLists.txt` now uses `kiko_add_core_test()` for test targets. Keep using helpers as tests grow;
do not add repeated `add_executable` / `target_link_libraries` / `add_test` blocks.

## Recommended transfer refactors

`src/receive_plan.*` now owns receive preflight, collision detection, conflict action selection,
manifest/header validation, and planned resume offsets. `src/transfer_manifest.*` owns manifest
encoding/decoding, and `src/transfer_resume.*` owns resume frames, prefix hashing, `.kikopart`
helpers, and completed-file fast-skip. Continue splitting `src/transfer_stream.cpp` in this order:

1. Consider extracting receive path/finalize helpers only if the next transfer feature touches them.
2. Keep `src/transfer_stream.cpp` as the single-stream send/receive coordinator.

Stop before moving directories unless a future feature needs it; small focused extractions create
less merge noise than a full tree reshuffle.

## Recommended TUI refactors

The transfer progress view has been split into `src/tui_transfer_view.*`, the network check modal
has been split into `src/tui_doctor_modal.*`, menu loading/saving helper state lives in
`src/tui_menu_state.*`, transfer launch parameters live in `src/tui_transfer_spec.hpp`, and
worker-thread transfer execution now lives in `src/tui_session.*`. Transfer completion actions
live in `src/tui_transfer_actions.*`, and menu rendering/control layout lives in `src/tui_menu_view.*`.
Only continue splitting `src/tui.cpp` if a future TUI feature needs it:

1. Expand `tui_menu_state.*`: remaining menu rendering helper labels and form-only decisions.
2. Expand `tui_session.*`: retry, return-to-menu, quit confirmation.
3. Consider stopping here unless a future TUI feature needs another seam. `src/tui.cpp` is now
   small enough to act as the menu/session coordinator.

Keep `src/tui.cpp` as the entry glue for `run_tui_send`, `run_tui_recv`, and `run_tui_menu`.
The target size for `src/tui.cpp` after this split is roughly 150-250 lines.

## Testing expectations

- Core networking and transfer behavior should stay covered by CTest unit/integration tests.
- UI helper modules should expose pure validation/mapping functions where possible.
- Full TUI rendering remains covered by `scripts/tui_smoke.py` on non-Windows platforms.
- Installer and release behavior belongs in GitHub Actions, not local unit tests.
