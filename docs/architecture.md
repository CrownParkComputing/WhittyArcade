# WhittyArcade architecture

WhittyArcade separates arcade hardware from application and host-output
policy. A ROM change destroys the complete emulated cabinet but deliberately
keeps the host video worker alive.

## Registration and routing

- `arcade_catalog.cpp` is the lightweight source of truth for board names,
  menu order, ROM-library directories and supported game manifests. It does
  not link any ROM loader.
- `arcade_game_probe.cpp` asks each board loader to identify archive contents
  and returns a stable MAME short name plus board type.
- `rom_library.cpp` owns collection scanning, companion archives, import and
  audit. It consumes catalog metadata rather than maintaining another game
  list.
- `arcade_session.cpp` is the only board-to-session factory switch.

The launcher, ROM importer, EEPROM manager and high-score viewer all consume
the catalog. Adding a game must not require another menu switch.

## Runtime layers

1. `main.cpp` owns command-line tools, frame pacing and the restart loop. It
   has no board ROM enums, DIP layouts or machine constructors.
2. `arcade_session.h` defines the application-facing session contract.
   `arcade_session_internal.h` implements common video/menu/pause forwarding.
3. One `*_session.cpp` module wires each hardware family to input, audio,
   operator settings and host video.
4. Board machine, CPU, audio, geometry and ROM modules contain hardware
   behaviour and do not select other boards.
5. `arcade_video_worker` exclusively owns SDL and the active OpenGL, Vulkan
   or software presenter for the life of the process.

## Session lifetime

On a ROM switch the outgoing session stops board-owned producers first,
fences and clears queued video work, removes its input watch, flushes durable
operator data, and then releases CPUs/RAM. The next board is constructed from
fresh objects. The shared video worker and its window survive, avoiding driver
teardown while preventing RAM, audio queues or CPU state from leaking between
games.

`arcade_session_factory_test` repeatedly constructs and destroys every
catalogued board against one shared video worker. The application also has a
separate live-video session-boundary test.

## Thread ownership

- System 22 advances the MC68020, both C71 DSPs and C74 in deterministic
  bounded slices. C352/OpenAL and host presentation have dedicated owners.
- Model 1 keeps V60/TGP scheduling deterministic, uses a bounded graphics
  worker for display-list rasterisation, and owns its 68000/YM3438/MultiPCM
  devices on the sound worker.
- Model 2 exclusively owns i960/TGP/geometry state on its CPU worker; its
  68000/SCSP sound board runs independently on the audio worker.
- Phoenix, Galaxian, Moon Cresta, UniWar S and Shinobi submit complete
  immutable RGBA frames to the same presentation worker. Their audio workers
  never own machine RAM.

Emulated processors are not placed on unconstrained host threads when they
share timing-sensitive RAM or interrupts. Parallelism follows hardware-safe
handoff boundaries.

## Stable contracts

- MAME short names are durable keys for imports, EEPROM/NVRAM and scores.
- A loader's `set_short_name()` must resolve to exactly one catalog manifest.
- Every board type has exactly one board descriptor and at least one game.
- Unknown archives never fall through to System 22.
- A not-working manifest can be discovered and explained but not launched.
- Persistent files are generated from catalog metadata for boards with a
  known storage layout; unverified EEPROM bytes are never given guessed names.

These invariants are enforced by unit tests and real-ROM integration tests.
