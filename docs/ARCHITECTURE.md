# kiko architecture

`kiko` is currently a single CMake project with one core static library, one CLI/TUI/Web executable,
and one lightweight `kiko-relayd` executable for server relay deployment. The source tree has moved
from a small flat layout into explicit module directories; future work should preserve those
boundaries instead of reintroducing cross-layer feature files.

## Current modules

- `app`: CLI entry points for `kiko` and `kiko-relayd`.
- `core`: protocol framing, sockets, structured network-interface inventory, crypto, compression, PAKE, QR output,
  progress events, and shared progress state.
- `connect`: shared peer connection options, peer/session setup, relay/direct route selection, LAN discovery,
  LAN upgrade, UDP punch, profile memory, STUN/NAT route planning, encrypted session helpers, and the shared
  peer route lifecycle.
- `relay`: rendezvous relay server, relay control path, relay room state, and relay route state.
- `transfer`: send/recv orchestration, shared per-file lifecycle, stream and mux payload adapters,
  resume, receive planning, metadata, symlink handling, and transfer heuristics.
- `note`: shared notepad protocol, encrypted peer session, thread-safe workspace state, and CLI adapter.
- `tui`: FTXUI menu/progress screens, path browser UI, transfer actions, doctor modal, and notepad UI.
- `web`: loopback-only Web console, embedded assets, local filesystem browser API, Web reporter,
  HTTP adapters, and single-task job orchestration for transfer, doctor, and notepad.
- `platform`: runtime user config, shared filesystem browser model, and platform shims.
- `diagnostics`: doctor, network probes, outbound relay path probing, and BYOK AI route advice.
- `release`: GitHub Actions release packaging and installer smoke tests.

Tests still live in a mostly flat `tests/` directory, with script-based smoke tests under `scripts/`.
That is acceptable for now; production code should remain in the layer directory that owns the behavior.

## Friction points

### Web modules

Web responsibilities now have three modules:

- `src/web/web.cpp`: loopback HTTP parsing, token checks, JSON/config mapping, and API responses.
- `src/web/web_job.*`: task admission, cancellation, snapshots, reporter events, and worker ownership.
- `src/web/web_assets.*`: embedded HTML/CSS/JavaScript.

Notepad transport/workspace state lives in `src/note/*`, and filesystem listing/filtering/sorting lives
in `src/platform/path_browser.*`. This split is deep enough for the current local console. Avoid
splitting HTTP helpers into pass-through modules unless another HTTP server surface needs them.

### Transfer coordinator size

`src/transfer/transfer_stream.cpp` delegates receive-plan preflight to `src/transfer/receive_plan.*`,
manifest JSON encode/decode to `src/transfer/transfer_manifest.*`, resume negotiation / `.kikopart`
helpers to `src/transfer/transfer_resume.*`, and receive path/finalize helpers to
`src/transfer/transfer_receive_paths.*`. `src/transfer/transfer_file_session.*` owns the per-file
control lifecycle shared with mux: headers, markers, conflict decisions, resume negotiation, source
hashing, final verification, metadata, and progress events. `transfer_stream.cpp` should remain the
single-stream payload adapter and coordinator.

### Flat source tree

The current source directories are the intended layering target:

```text
src/
  app/          main.cpp, relayd_main.cpp
  core/         common, io, socket, crypto, pake, protocol, cancellation
  connect/      peer_options, connectivity, route_session, route_planner, direct_session, udp_punch, peer_session
  relay/        relay_server, relay_room_state, relay_route_state
  transfer/     transfer, file lifecycle, stream/mux payload adapters, receive_plan, manifest, resume
  note/         notepad protocol, session, workspace, and CLI adapter
  tui/          TUI menu, transfer, browser, doctor, notepad
  web/          loopback HTTP adapter, job orchestration, and embedded browser UI
  platform/     path_browser, user_config, and platform shims
  diagnostics/  doctor, AI advisor, network_probe, outbound_policy
```

Avoid another broad directory reshuffle unless ownership boundaries change; most next improvements
should be small extractions inside these directories.

### CMake growth

`CMakeLists.txt` now uses `kiko_add_core_test()` for test targets. Keep using helpers as tests grow;
do not add repeated `add_executable` / `target_link_libraries` / `add_test` blocks.

## Recommended transfer refactors

`src/transfer/receive_plan.*` now owns receive preflight, collision detection, conflict action selection,
manifest/header validation, and planned resume offsets. `src/transfer/transfer_manifest.*` owns manifest
encoding/decoding, and `src/transfer/transfer_resume.*` owns resume frames, prefix hashing, `.kikopart`
helpers, and completed-file fast-skip. `src/transfer/transfer_receive_paths.*` owns safe output paths,
conflict rename helpers, symlink creation, and part-file verification/finalization.
`src/transfer/transfer_file_session.*` owns the shared file transaction around those helpers.

Keep `src/transfer/transfer_stream.cpp` and `src/transfer/mux_transfer.cpp` as payload adapters:

1. Stream owns stateful zstd compression/decompression and ordered reads/writes.
2. Mux owns chunk offsets, channel scheduling, random writes, and contiguous-prefix tracking.
3. Do not add a generic payload-writer callback or template over both loops; their scheduling semantics
   are intentionally different.
4. Only extract tagged frame helpers if another transfer mode needs them without either payload loop.

Stop before moving directories unless a future feature needs it; small focused extractions create
less merge noise than a full tree reshuffle.

## Recommended TUI refactors

The transfer progress view has been split into `src/tui/tui_transfer_view.*`, the network check modal
has been split into `src/tui/tui_doctor_modal.*`, menu loading/saving helper state lives in
`src/tui/tui_menu_state.*`, transfer launch parameters live in `src/tui/tui_transfer_spec.hpp`, and
worker-thread transfer execution now lives in `src/tui/tui_session.*`. Transfer completion actions
live in `src/tui/tui_transfer_actions.*`, menu rendering/control layout lives in
`src/tui/tui_menu_view.*`, and shared notepad UI lives in `src/tui/tui_note.*`.
Only continue splitting `src/tui/tui.cpp` if a future TUI feature needs it:

1. Expand `tui_menu_state.*`: remaining menu rendering helper labels and form-only decisions.
2. Expand `tui_session.*`: retry, return-to-menu, quit confirmation.
3. Consider stopping here unless a future TUI feature needs another seam. `src/tui/tui.cpp` is now
   small enough to act as the menu/session coordinator.

Keep `src/tui/tui.cpp` as the entry glue for `run_tui_send`, `run_tui_recv`, and `run_tui_menu`.
The target size for `src/tui/tui.cpp` after this split is roughly 150-250 lines.

## Recommended Note/Web boundaries

Notepad intentionally avoids system clipboard integration. Keep clipboard backends out of the tree
unless that feature is deliberately restarted with a separate design.

- `src/note/note_protocol.*` owns encrypted frame encoding, validation, and last-write-wins document updates.
- `src/note/note_session.*` owns peer setup, hello exchange, send queue, receive loop, ACK handling,
  workspace composition, semantic workspace events, and cancellation.
- `src/note/note_workspace.*` owns pads, active selection, revisions, and per-pad sync state.
- `src/note/notepad.*` owns the non-TTY CLI interaction only.
- `src/tui/tui_note.*` owns terminal editing behavior, debounce, cancel, and TUI status.
- `src/web/*` owns browser controls, token-gated local APIs, and mapping workspace snapshots to JSON.
- `src/web/web_job.*` owns Web task lifecycle and maps `NoteSession` snapshots into job snapshots.
- `src/connect/peer_session.*` is the reusable encrypted peer-session helper for non-file features.
- `src/connect/peer_route_session.*` owns pairing-code setup, listener/embedded-relay lifetime, LAN discovery,
  outbound selection, STUN, rendezvous, NAT facts, route selection, encryption, mux channel establishment,
  cancellation tracking, and route-profile recording shared by file transfer and Notepad.
- `src/relay/relay_protocol.*` owns the typed relay hello and peer-info wire contracts, including defaults,
  port validation, connection-count limits, extension-field preservation, and canonical field encoding.
- `src/relay/relay_server.*` owns relay socket handling and room/route execution; it should consume the typed
  relay protocol instead of parsing or constructing rendezvous fields in place.

Do not add note-specific route selection or relay logic to `src/note/*`; route setup belongs in
`src/connect/*`. `NoteSession` configures and consumes `PeerSession` without reimplementing route
selection.

`src/connect/peer_options.hpp` owns connection fields shared by Send, Recv, and Notepad. CLI, TUI,
and Web adapters should map their raw inputs into `PeerConnectionOptions` once, then apply only
feature-specific fields. Keep Doctor independent because its probe-only options do not have peer
listener, LAN discovery, pairing timeout, or session cancellation semantics.

`src/core/progress_state.*` owns the transfer progress snapshot, bounded event log, terminal and
retry transitions, and human-readable route phase/outcome/timing text shared by TUI and Web.
Frontends retain only their synchronization, rendering, QR, JSON, Doctor, and Notepad state.

`PeerSession` is intentionally a small single-channel adapter over `PeerRouteSession`. File transfer consumes
the same Module with file-specific hello fields, AI route hints, mux width, and payload dispatch. Keep those
file policies in `transfer/*`; do not move manifest, conflict, resume, or transfer heuristics into `connect/*`.

## Shared path browser boundaries

- `src/platform/path_browser.*` owns directory normalization, listing, file/directory selection modes,
  case-insensitive filtering, name/modified-time sorting, and special parent/select-folder entries.
- `src/tui/tui_browser.*` owns FTXUI navigation, cached filtering, selection, and cancel behavior.
- `src/web/*` owns query parsing and JSON serialization for `/api/fs`.

Keep filesystem ordering and filtering changes in `path_browser.*` so Send and Recv behave the same in
TUI and Web.

## Network interface inventory

- `src/core/network_interfaces.*` owns one structured snapshot of active interface addresses and derives
  non-loopback addresses, non-VPN LAN candidates, VPN presence, and the preferred physical interface.
- Connectivity snapshots, outbound relay selection, Doctor, profile fingerprints, and peer setup should
  reuse one inventory per operation instead of scanning platform interfaces independently.
- Keep interface-name heuristics and platform enumeration in this module so VPN and physical-interface
  decisions cannot drift between connection and diagnostics paths.

## Testing expectations

- Core networking and transfer behavior should stay covered by CTest unit/integration tests.
- UI helper modules should expose pure validation/mapping functions where possible.
- Full TUI rendering remains covered by `scripts/tui_smoke.py` on non-Windows platforms.
- Installer and release behavior belongs in GitHub Actions, not local unit tests.
