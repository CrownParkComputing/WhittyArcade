# WhittyArcade

WhittyArcade is a standalone multi-board arcade emulator. It currently covers
Namco System 22, Sega Model 1, Sega Model 2, Phoenix hardware, Galaxian
hardware and Sega System 16B. MAME's BSD hardware implementations are the
primary behaviour reference; machine integration, scheduling, frontend,
storage and host output are standalone.

## Current status

Implemented and tested:

- Motorola MC68020 execution through the standalone Musashi 4.60 core
- Big-endian non-Super System 22 main memory map foundation
- System 22 interrupt controller and game-programmable vblank IRQ delivery
- Directory and ZIP ROM loading with MAME sizes and CRC-32 validation
- Per-game program interleave, ROM-region placement and CRC validation
- Game-specific C370 KEYCUS responses and C139 standalone link status
- C71 and C74 internal firmware loading from `namcoc71.zip`/`namcoc74.zip`
- Two real TMS320C25-derived C71 cores with master/slave program, data, I/O,
  point-ROM and banked polygon-RAM maps
- Real M37702 C74 execution with internal peripherals, Timer A0, shared RAM,
  external sequence/code ROM and the System 22 sound-role port selection
- 32-voice C352 mixing: PCM, mu-law, noise, interpolation, volume ramps,
  forward/reverse looping, phase flags and key events
- Real C74-to-C352 register traffic over a bounded SPSC queue, with C352
  mixing and OpenAL streaming owned by a dedicated audio thread
- OpenGL 4.3 renderer lifecycle, event handling and a thread-safe polygon
  producer/consumer boundary
- C71 direct-render command decoding, priority sorting and hardware triangle
  rendering for the resulting neutral polygon records
- Ridge Racer's linked PDP command stream, point-ROM/point-RAM object lists,
  model/view transforms, lighting, culling and representative-Z priorities
- GPU integer-texture indirection for Ridge Racer's raw 16x16 texels, 1M-entry
  tilemap, packed transform attributes, color modes and live palette RAM
- GPU text layer from live 16x16x4bpp character RAM and 64x64 tile RAM, with
  scrolling, flip attributes, palette banking, transparent pen and per-pixel
  polygon-over-character priority
- RGBA8 scene composition with the non-Super System 22 global 8.8 fade and
  final red/green/blue gamma-PROM lookup
- Non-Super System 22 direct-polygon CZ table selection and per-type fog color
- Hot-pluggable SDL/Xbox and keyboard controls for driving, twin-stick and 2D
  cabinets, with per-device launcher mapping, calibrated axes and debounced
  coin/start lines
- Framework-free NEC V60 and Fujitsu MB86233 execution, with ROM-backed Model
  1 geometry processing, arithmetic tables, shared RAM, FIFOs, interrupts and
  ROM-bank machinery for Virtua Formula
- System 24 RAM tile pages, character decoding, split/window modes and correct
  backdrop -> 3D -> HUD composition for the Model 1 video path
- Native Model 1 support for Virtua Formula, Virtua Fighter, Star Wars Arcade
  and Wing War, including per-game ROM maps and sound-board variants
- Native Model 2 support for Sega Rally Championship with i960, geometry,
  SCSP audio, inputs, NVRAM and race-frame regression coverage
- Phoenix, Moon Cresta and Shinobi machine/video/input/audio paths using the
  same board-neutral session and presentation contracts
- Shared scaling, filtering, settings, overlays and ROM menus across OpenGL,
  Vulkan-transfer and SDL software presentation

The supported sets boot the MC68020, both C71s and C74 together and reach their
attract/gameplay rendering. The master C71 emits direct commands plus complete
PDP point-ROM scenes every frame, while the C74 runs the game sound program and
programs C352. The polygon, text, priority and final-mix shader pipelines have
been exercised on a real OpenGL 4.3 driver.

Not implemented yet:

- Remaining slave-C71 render-device packet variants beyond Ridge Racer's
  validated PDP stream
- Remaining CZ/fog edge cases and sprite support for later System 22 titles
- Full external I/O-C74 serial protocol and force-feedback/output devices
- Full KEYCUS behaviour, EEPROM timing, and the remaining non-vblank IRQs
- Model 1 force feedback, network board and cycle-level I/O-board Z80
  execution

The current binary remains experimental; the limitations above affect hardware
accuracy and cabinet peripherals rather than basic startup and rendering.

Ridge Racer Full Scale is recognized but marked **not working**: its video ROM
dump and linked-cabinet display path are incomplete, so the emulator prevents
launching it rather than opening a black screen.

## Thread ownership

- The System 22 scheduler advances the MC68020, both C71 DSPs and C74 in 64
  bounded deterministic cycle slices per video frame. Model 1 likewise keeps
  its V60 and 40 MHz MB86233/TGP in 512 deterministic slices so FIFO/shared-RAM
  barriers cannot race on the host.
- CPU/DSP code submits renderer-neutral polygon records without making OpenGL
  calls. The OpenGL context thread owns upload and draw operations.
- Model 1 display RAM is snapshotted at the hardware frame boundary into a
  bounded double-buffered queue. A dedicated graphics worker exclusively owns
  display-list decode, clipping and software rasterization while the V60/TGP
  producer advances the next frame. Completed immutable RGBA buffers cross
  back to the OpenGL presentation thread without sharing renderer state.
- The System 22 audio thread owns C352 state and OpenAL. C74 register writes cross a
  bounded release/acquire command queue; the emulation thread never waits for
  the audio device.
- Model 1 uses a separate sound worker which owns its 10 MHz 68000, 8 MHz
  YM3438, both 10 MHz MultiPCM chips, continuous native-rate resamplers and
  OpenAL stream. V60 UART bytes cross a bounded SPSC queue; this board does
  not share System 22's incompatible C352 implementation.
- The Model 2 sound board (11.29 MHz 68000 + SCSP) runs on its own audio
  thread clocked by the OpenAL stream itself, matching the real board's
  independence from the geometry side: music, speech and SFX stay in real
  time even when the i960/TGP frame rate drops. MIDI crosses lock-free
  queues in both directions at the hardware's 31.25 kbaud serial pacing.

Running emulated processors freely on independent host threads is deliberately
avoided: System 22 shared-memory and interrupt timing need deterministic
synchronisation before CPU parallelism is safe.

## Build

Dependencies: CMake 3.16+, SDL2, SDL2_ttf, OpenGL 4.3, Vulkan, GLEW, GLM,
OpenAL, zlib, MiniZip, libmpg123 and a C/C++17 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The `CachyOS build` GitHub Actions workflow performs the same release build and
test suite in the official rolling Arch Linux container, matching CachyOS's
glibc and pacman ecosystem. Successful runs publish a
`WhittyArcade-cachyos-x86_64` artifact containing the executable, README and
SHA-256 checksum.

## Run

With no arguments, the launcher opens the board and ROM-library menu. The first
argument may still be an extracted supported set or compatible ZIP. The second
argument, used by System 22 sets, is a directory containing the C71/C74
firmware archives or loose firmware files.

```bash
./build/WhittyArcade /path/to/ridgerac.zip /path/to/bios
./build/WhittyArcade /path/to/vformula.zip
```

## ROM library and MAME archive layouts

The launcher has a **ROM Library / Import** page. It can import selected ZIPs
or scan a complete MAME ROM folder. Imported archives are copied unchanged to
`$XDG_DATA_HOME/WhittyArcade/roms` (normally
`~/.local/share/WhittyArcade/roms`) and grouped into board subdirectories.
WhittyArcade never extracts, modifies or repacks ROM contents, so the imported
files remain usable by MAME. The original download can safely be removed after
importing.

The same importer is available without the GUI:

```bash
./build/WhittyArcade --import /path/to/mame/roms
./build/WhittyArcade --import game.zip parent.zip
./build/WhittyArcade --list-roms
./build/WhittyArcade --audit-roms
```

All three standard MAME layouts are supported:

- **Non-merged:** select/import the game ZIP; it already contains its shared
  ROMs.
- **Split:** keep the listed parent or companion ZIP beside the game ZIP.
  Loaders search the selected archive first and then the exact companion.
- **Merged:** select/import the parent archive. Entries in clone
  subdirectories are found by basename.

Required MAME short names for this build:

| Board | Games | Additional archives |
|---|---|---|
| Namco System 22 | `ridgerac`, `ridgera2`, `raverace`, `acedrive`, `victlap`, `cybrcomm` | `namcoc71.zip`, `namcoc74.zip` |
| Sega Model 1 | `vformula`, `vf`, `swa`, `wingwar` | Split `vformula` also needs `vr.zip`; merged `vr.zip` works directly |
| Sega Model 2 | `srallyc` | None (`segabill.zip` is optional) |
| Phoenix hardware | `phoenix` | None |
| Galaxian hardware | `mooncrst` | None |
| Sega System 16B | `shinobi4` | Split collections also need `shinobi6.zip` for the unencrypted sound program; merged `shinobi.zip` works directly |

`ridgeracf` (Ridge Racer Full Scale) is detected and shown as not working; its
incomplete video/linked-cabinet dump is not launchable. Clone revisions not
listed above are not silently substituted for the supported program revision.

Besides the durable library, discovery checks the explicitly selected path,
its directory, the legacy `~/Downloads/WhittyArcade-Roms` tree and ZIPs placed
directly in `~/Downloads`. Extra read-only library roots can be supplied as a
colon-separated `WHITTYARCADE_ROM_PATH`.

Expected firmware:

- `c71.bin`: 8192 bytes, CRC `47c623ab`
- `c74.bin`: 16384 bytes, CRC `a3dce360`

The firmware may be loose or stored in `namcoc71.zip` and `namcoc74.zip`.

## Controls

Open **Controllers / Keyboard** from the main menu to view every plugged-in SDL
game controller and the keyboard. Choose an arcade action, then press a key,
button or move an axis; Delete/Backspace clears that action. Controller maps are
stored by SDL device GUID, so reconnecting the same controller model restores
its profile. A Refresh row detects devices plugged in while this page is open.

Default Xbox-style controller map:

- Left stick: steering / Player 1 directions
- Right stick: twin-stick secondary directions
- Right trigger: accelerator
- Left trigger: brake
- View/Back: coin 1
- Menu/Start: start 1
- Left/right shoulder: shift down/up
- A/B/X: Player 1 actions 1/2/3
- B/X/Y/A: driving views 1/2/3/4

Default keyboard map:

- Left/Right: steering; Up: accelerator; Down: brake
- WASD: Player 1 directions; Z/X/C: Player 1 actions
- Arrow keys: twin-stick secondary directions
- I/J/K/L: Player 2 directions; U: Player 2 action 1
- `5`: coin 1; `6`: coin 2; `9`: service credit; `F2`: test switch
- `1`: start 1; `2`: start 2; Z/X: shift down/up; V/B/N/G: views 1-4
- `Esc`: return from gameplay to the main menu; from the main menu, exit
- `R`: open/close the board-grouped ROM selector; choosing a
  recognized ZIP performs a clean machine reset
- `F8`: edit all 16 cabinet DIP switches (SW2:1-8 and SW3:1-8)
- `S`: open/close emulator settings: master/music/effects volume, fullscreen,
  window size, VSync, integer scaling, filtering, and ROM selection
- Ctrl+Plus/Ctrl+Minus: resize the window from 1x to 4x; Alt+Enter toggles
  borderless fullscreen

Settings are saved to
`~/.config/WhittyArcade/settings.ini` (or `$XDG_CONFIG_HOME`). Existing
`ridge_racer_emulator` settings are imported automatically. Music
volume controls looping C352 voices; effects/speech volume controls one-shot
voices. Every board uses the same programme-loudness target and peak limiter;
the master volume is applied afterwards as the global listening level.
Input mappings are saved separately in
`~/.config/WhittyArcade/input.ini` (or `$XDG_CONFIG_HOME`) and are reloaded for
each newly launched cabinet.

## EEPROM, NVRAM and high scores

The launcher's **EEPROM / NVRAM Manager** shows every persistent chip per game,
including its expected size and last modification time. It can create dated
backups, export a portable save folder, validate and restore an import, or
perform a recoverable factory reset. Existing data is backed up before restore
or reset.

Operator data lives below `$XDG_CONFIG_HOME/WhittyArcade/nvram` (normally
`~/.config/WhittyArcade/nvram`):

- System 22: one MAME-style 8192-byte `eeprom` file per set.
- Model 1: one 128-byte operator EEPROM (`vformula.nv`, `vf.nv`, `swa.nv`, or
  `wingwar.nv`).
- Model 2 Sega Rally: a 128-byte `eeprom` plus 16 KiB `backup1` battery SRAM.

System 22 service settings now persist across launches instead of being
reloaded from the ROM image every time.

The **High Scores** page displays verified score tables for Phoenix, Moon
Cresta and Shinobi without launching the game. WhittyArcade saves these as
MAME-compatible `.hi` files below
`$XDG_CONFIG_HOME/WhittyArcade/hi` (normally
`~/.config/WhittyArcade/hi`) and also detects existing files in MAME's usual
`~/.mame/hi` and `~/.local/share/mame/hi` folders. A score file is restored
only after the game initializes its default table. Other games remain clearly
marked as not decoded; raw EEPROM bytes are never presented as guessed scores.

On Hyprland the emulator uses the stable `WhittyArcade` window class
and should be floated with an app-specific window rule. Tiled clients cannot
honor application-requested 1x-4x window sizes by compositor design.

The settings panel reports the active graphics API. Board rasterisation is
performed once into a complete frame; OpenGL presents it directly, Vulkan uses
a native swapchain transfer path, and software uses SDL's CPU renderer. All
three use the same source dimensions, aspect fit, integer-scaling policy and
overlay layout. A live backend change preserves the emulated machine and
recreates only host presentation resources.

The normal DIP configuration is all OFF. SW2:1 enters hardware test mode;
Ridge Racer 2 also exposes documented/reverse-engineered graphics, link, timer
and secondary-test debug switches. Coinage, difficulty, laps and sound options
belong to the in-game service menu rather than the physical DIP banks.

Ace Driver and Victory Lap use the same steering controls, with their shorter
cabinet pedal ranges calibrated automatically. Cyber Commando uses both
controller sticks; on keyboard, WASD controls the left stick and the arrow keys
control the right stick. Z/X operate gun trigger/missile and V changes view.

Ridge Racer's standard cabinet input map has no separate start switch. After
inserting a coin, press the accelerator to start/select. Controllers can be
connected while the emulator is running. Set `RRACER_CONTROLLER=N` to prefer a
specific SDL joystick index.

Virtua Formula uses `1`/controller Start for its cabinet start switch. `5`
inserts a coin; steering and pedals use the same calibrated host controls as
the System 22 driving games.

For controller diagnosis, set `RRACER_INPUT_TRACE=1`; the emulator prints the
switch word, three calibrated ADC values and credit counters once per second.

## Source layout

- `src/arcade_catalog.cpp` — canonical board and supported-game metadata
- `src/arcade_game_probe.cpp` — content-based ROM-to-board identification
- `src/arcade_session.cpp` — sole board-session factory
- `src/*_session.cpp` — per-board input/audio/operator/video runtime wiring
- `src/main.cpp` — board-neutral launcher, restart loop and frame pacing
- `src/system22_cpu.cpp` — MC68020 wrapper and System 22 main bus
- `src/system22_dsp.cpp` — dual-C71 maps, point memory and render transport
- `src/system22_mcu.cpp` — C74 map and C352 command producer
- `src/system22_rom.cpp` — validated game/firmware loading
- `src/system22_audio.cpp` — C352 and OpenAL thread
- `src/model1_audio.cpp` — Model 1 68000/YM3438/dual-MultiPCM sound worker
- `src/arcade_renderer.cpp` — shared renderer, menu and FPS overlay
- `src/arcade_video_worker.cpp` — board-neutral presentation worker
- `src/arcade_presenter.cpp` — Vulkan and software output paths
- `src/arcade_frontend.cpp` — catalog-driven launcher, ROM and operator tools
- `src/input_mapping.cpp` — logical actions, defaults and persistent profiles
- `src/input_mapper.cpp` — plugged-device selection and binding capture UI
- `src/arcade_input.cpp` — mapped SDL input, calibration and cabinet pulses
- `src/arcade_settings.cpp` — persistent audio/display settings
- `src/model1_rom.cpp` — validated Model 1 ROM loading/interleave
- `src/model1_machine.cpp` — V60 bus, interrupts, I/O mailbox and frame timing
- `third_party/model1` — standalone Model 1 geometry-board integration and
  Model 1/System 24 video
- `third_party/mb86233` — standalone Fujitsu geometry-processor core
- `third_party/musashi` — Musashi MC68020 core
- `third_party/m37710` — standalone M37702/M37710 CPU core
- `third_party/v60` — standalone NEC V60 CPU core
- `tests` — catalog/lifecycle, ROM, CPU/bus, audio, video and boot regressions

See [architecture](docs/architecture.md) and
[adding games and boards](docs/adding_games_and_boards.md) for extension
contracts and the required test path.

## Credits and licensing

System 22 behaviour is based on MAME's BSD-3-Clause `namcos22`, C352,
TMS320C2x and M37710 implementations by Phil Stroffolino, hap, R. Belmont,
superctr, Tony La Porta and other MAME contributors. The V60, Model 1 TGP and
video adaptations retain their BSD-3-Clause notices and Olivier Galibert's
copyright. Musashi is by Karl Stenerud under its permissive license. Source
files retain their applicable copyright and license notices.
