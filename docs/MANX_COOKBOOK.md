# MANX: end-to-end arcade platform cookbook

MANX is a multi-board arcade platform, so readiness is the intersection of
shared host gates and a separate evidence record for every supported game and
board. One working title cannot certify another title, even on the same board.

## 0. Legal, licence and repository boundary

- Users supply only ROMs, firmware and CHDs they are legally entitled to use.
- Never stage or publish ROMs, firmware, keys, CHDs, EEPROM/NVRAM, save data,
  extracted artwork, credentials, private media packs or generated binaries.
- Preserve GPLv3 and every third-party notice. Record the exact upstream file,
  revision and licence for imported emulator components.
- Keep owned-media paths in the local CMake cache or command line, never source.

Before every commit inspect `git status --short`, `git diff --cached --check`
and the complete staged file list. The root `.gitignore` is part of the gate,
not a substitute for reviewing the index.

## 1. Product identity and host lifecycle

- Build the MANX executable from a clean checkout on every supported desktop
  platform and keep the exact compiler/dependency revisions.
- Prove launcher start, ROM-library scan, game create, repeated frame execution,
  pause/resume, reset, game switch, return to launcher and final destruction.
- The OS close action must be distinct from in-game Back. Stop board producers,
  audio, input, renderer, link transport and plugins in a deterministic order.
- A bounded GUI integration test must own the exact launched PID/window, request
  normal close, require status zero and prove no child/cabinet process remains.

## 2. Shared unit and integration gates

Run the complete no-ROM CTest suite. Every shared fix needs a small semantic
unit test plus the affected board integration route. The baseline command is:

```sh
ctest --test-dir build-clean --output-on-failure -j 1
```

C139 and Model 2 link tests open localhost UDP ports and must run in an
environment that permits loopback networking. A denied socket is an environment
failure, but must be rerun with loopback enabled rather than marked passed.

Shared coverage must include session lifecycle/factory, audio output and PCM
queues, renderer/post-processing, input mapping, settings, media and ROM
libraries, persistence/high scores, plugin load/unload, cabinet layout, LAN and
internet protocol state, and clean shutdown.

## 3. Per-game ROM identity

For each catalog entry retain a machine-readable manifest containing:

- durable MAME short name and parent/device dependencies;
- filename, region, byte size, CRC-32 and SHA-1 for every member;
- interleave, byte order, transforms, bank/protection data and optional media;
- board/CPU/video/audio/input profile and supported cabinet/link topology.

Test complete load, missing member, wrong size, wrong hash, wrong parent, corrupt
archive and a valid merged/non-merged arrangement where applicable. Never let a
partial set boot as if it were complete.

## 4. CPU, board and timing

Each board needs no-ROM contract tests for its maps, interrupts, timers, DMA,
banks, protection, EEPROM and coprocessors, plus optional owned-ROM boot tests.
Measure the real board clocks and frame cadence; verify long-run bounded pacing,
audio/video synchronisation and 1% lows. A title-screen attract loop is not a
substitute for interactive gameplay and result/return flow.

## 5. Rendering

For every graphical defect retain the preceding good frame, first wrong frame,
relevant emulated state and a replayable scene. Verify palette/CLUT, sprites,
tilemaps, polygons, priority, blending, fog, depth, clipping, viewport, texture
formats and presentation ownership in the board layer.

Per-title routes must cover attract/title, gameplay start, dense effects,
camera extremes, HUD/overlays, pause, result/game-over and return. Cold/warm
shader and artwork caches are separate routes. Never conceal a board defect
with a launcher overlay.

## 6. Input, cabinet I/O and feedback

- Verify keyboard and controller mappings, analog calibration/dead zones,
  digital edges, coin/start/service/test and per-player assignment.
- Test disconnected/reconnected devices, hot-plug, focus loss and remapping.
- For driving/lightgun/link cabinets test the actual wheel/gun/gear/feedback or
  link state observed by the board, not only host events.
- Linked cabinets need single, master/slave and failure/rejoin routes. Close one
  cabinet and prove the whole session reaches a defined clean state.

## 7. Audio

Unit tests must cover mix/queue semantics and board-device state. Owned-ROM
integration must retain non-zero signal evidence through attract, gameplay and
result; check music, effects, speech, mute/volume, focus loss, pause/resume,
underrun, stuck loops and shutdown. Silence submitted successfully is not a
passing audio result.

## 8. Persistence, achievements and network

- Use isolated temporary roots for unit/integration tests.
- Test missing, existing and corrupt EEPROM/NVRAM/high-score/settings state,
  then reload it in a second process and prove guest-visible state.
- Test plugin achievements and cloud-save conflict/retry without production
  credentials or destructive writes.
- Network tests use deterministic loopback/local fixtures by default; live
  services require an explicit separate gate and revocable test credentials.

## 9. Per-title release matrix

Every catalog title gets a status record with these states: `not-tested`,
`partial`, `pass`, `blocked`, or `not-applicable`.

| Gate | Required proof |
|---|---|
| Provenance | exact owned ROM/media inventory and source boundary |
| Loader | positive set plus missing/wrong/corrupt rejection |
| CPU/board | boot and board-device contract tests |
| Timing | measured cadence, pacing and 1% lows |
| Input/I/O | full controls, service path and hot-plug |
| Graphics | attract, gameplay, dense scene, result and cache routes |
| Audio | non-zero signal and transition tests |
| Persistence | cold/write/second-process reload/corrupt recovery |
| Link/network | applicable cabinet topology and failure handling |
| Shutdown | normal close, status zero and no residual processes |
| Regression | no-ROM CTest plus available owned-ROM tests |

## 10. Current baseline

As of 2026-08-12, the existing `build-clean` configuration exposes 60 no-ROM
tests. All 60 pass when localhost networking is enabled. In the network-isolated
sandbox, 57 pass and the three loopback-dependent C139/Model 2 I/O tests fail at
socket setup; rerunning those three with loopback access passes.

This certifies the current shared test baseline only. It does not certify every
catalog title, owned-ROM deep boot/gameplay, GUI process closure, Windows, or
release packaging. Those remain explicit gates in `docs/manifests/test-status.toml`.
