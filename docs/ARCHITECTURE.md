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

### TUI file size

`src/tui.cpp` is the largest file and carries too many responsibilities:

- menu form state and validation
- transfer progress state
- worker thread lifecycle
- doctor modal
- render functions
- send/recv config assembly
- preference saving

This is the most important architecture cleanup target. New TUI behavior should avoid adding more
logic directly to `src/tui.cpp`.

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

## Recommended TUI refactors

The transfer progress view has been split into `src/tui_transfer_view.*`, the network check modal
has been split into `src/tui_doctor_modal.*`, menu loading/saving helper state lives in
`src/tui_menu_state.*`, transfer launch parameters live in `src/tui_transfer_spec.hpp`, and
worker-thread transfer execution now lives in `src/tui_session.*`. Transfer completion actions
live in `src/tui_transfer_actions.*`, and menu rendering/control layout lives in `src/tui_menu_view.*`.
Continue splitting `src/tui.cpp` in this order:

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
