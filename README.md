# WhittyArcade

WhittyArcade is a focused standalone multi-board arcade emulator for Windows
and Linux. Its 26 working sets span Namco System 22 / Super System 22, System
246, Model 1, Model 2, Namco System 1, Galaga, Galaxian, Phoenix, Sega System
16B, Ghosts'n Goblins hardware and an isolated offline Xbox 360 runtime.
MAME's BSD hardware implementations are the primary behaviour reference;
machine integration, scheduling, frontend, storage and host output are
standalone.

## Current status

Implemented and tested:

- Motorola MC68020 execution through the standalone Musashi 4.60 core
- Big-endian System 22 and Super System 22 main memory maps, including the
  later board's system controller, mixer, sprite/VICS, gun and point-RAM areas
- System 22 interrupt controller and game-programmable vblank IRQ delivery
- Directory and ZIP ROM loading with MAME sizes and CRC-32 validation
- Per-game program interleave, ROM-region placement and CRC validation
- Game-specific C370 KEYCUS responses and C139 standalone link status
- C71 and C74 internal firmware loading from `namcoc71.zip`/`namcoc74.zip`
- Two real TMS320C25-derived C71 cores with master/slave program, data, I/O,
  point-ROM and banked polygon-RAM maps
- Real M37702 C74 and M37710 execution with internal peripherals, Timer A0,
  shared RAM, external sequence/code ROM and board-specific port selection
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
- Super System 22 C374/VICS sprite-list decoding and 32x32x8bpp object tiles,
  with mixer-controlled polygon, sprite, text and background composition
- RGBA8 scene composition with the non-Super System 22 global 8.8 fade and
  final red/green/blue gamma-PROM lookup
- Non-Super System 22 direct-polygon CZ table selection and per-type fog color
- Time Crisis gun coordinates, cabinet trigger/pedal, mouse aiming, custom
  player-coloured crosshairs and its Super System 22 fade/gamma/CZ path
- Dirt Dash World DT2 Ver.C with its M37710 cabinet I/O, C418 protection,
  full C374/VICS sprite list, driving controls and stable projected shadows
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
- Phoenix, Galaxian, Moon Cresta, UniWar S and Shinobi
  machine/video/input/audio paths using the same board-neutral session and
  presentation contracts
- Shared scaling, filtering, settings, overlays and ROM menus across OpenGL,
  Vulkan-transfer and SDL software presentation

The base System 22 sets boot the MC68020, both C71s and C74 together and reach
their attract/gameplay rendering. Time Crisis and Dirt Dash boot their Super
System 22 MC68020, C71 pair and M37710, then reach attract and live gameplay
with 3D, sprites, text and C352 audio. The polygon, sprite, text, priority and
final-mix shader pipelines have been exercised on a real OpenGL 4.3 driver.

Not implemented yet:

- Remaining slave-C71 render-device packet variants beyond Ridge Racer's
  validated PDP stream
- Remaining CZ/fog edge cases and Super System 22 sprite alpha/blend details
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

- The System 22 scheduler advances the MC68020, both C71 DSPs and sound MCU in
  bounded deterministic cycle slices per video frame (64 on the base board;
  adaptive 512/256 during Super System 22 startup/runtime). Model 1 likewise keeps
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

Dependencies: CMake 3.18+, SDL3, SDL3_ttf, OpenGL 4.3, Vulkan, GLEW, GLM,
OpenAL, zlib, MiniZip, libmpg123 and a C/C++17 compiler.

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For fast System 246 iteration without launching a game or opening a window,
build and run only the ROM, controls, CD-stream, RRV music-decoder and SPU2
audio regressions:

```bash
cmake --build build --target check_system246
```

The RRV music regression reads the logical payload directly from either the
original CHD or the generated ISO. It can validate, export, or audibly play one
disc track at a time without booting the emulator:

```bash
./build/system246_music_test --list
./build/system246_music_test --verify /path/to/rrv1-a.chd 0
./build/system246_music_test --play /path/to/rrv1-a.chd 0
./build/system246_music_test --wav /path/to/rrv1-a.chd 0 rrv-bgm01.wav
```

GitHub Actions builds and tests native x86-64 artifacts for Ubuntu 26.04,
Debian 13, Fedora 43, openSUSE Tumbleweed and CachyOS/Arch on every push and
pull request. Each artifact contains the executable, licence, runtime notes
and a SHA-256 checksum. See `docs/building_windows_linux.md` for the package
names used by each distribution.

### Windows 10/11 x86-64

Open an **MSYS2 UCRT64** terminal, install the dependencies listed in
`docs/building_windows_linux.md`, then run:

```bash
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --parallel 2
ctest --test-dir build-windows --output-on-failure --parallel 2
```

The `Windows x86-64 build` workflow performs this native UCRT64 build on every
push and pull request. It publishes `WhittyArcade-windows-x86_64.zip` with
`WhittyArcade.exe`, all required non-system DLLs, licence files, runtime notes
and SHA-256 manifests. Debug symbols are retained as a separate CI
artifact. Windows uses its native file picker; no `zenity` or MSYS2 runtime is
required by the packaged executable.

## Run

With no arguments, the launcher opens the board and ROM-library menu. The first
argument may still be an extracted supported set or compatible ZIP. The second
argument, used by System 22 sets, is a directory containing the C71/C74
firmware archives or loose firmware files.

```bash
./build/WhittyArcade /path/to/ridgerac.zip /path/to/bios
./build/WhittyArcade /path/to/timecris.zip /path/to/bios
./build/WhittyArcade /path/to/dirtdash.zip /path/to/bios
./build/WhittyArcade /path/to/aquajet.zip /path/to/bios
./build/WhittyArcade /path/to/vformula.zip
./build/WhittyArcade /path/to/galaxian.zip
./build/WhittyArcade /path/to/uniwars.zip
./build/WhittyArcade /path/to/gng.zip
./build/WhittyArcade /path/to/galaga.zip
./build/WhittyArcade /path/to/pacmania.zip
```

## ROM library and MAME archive layouts

There is no ROM import step. WhittyArcade creates and reads two folders
directly:

- Linux: `~/.local/share/WhittyArcade/roms` and
  `~/.local/share/WhittyArcade/chd` (or the equivalent below
  `$XDG_DATA_HOME`).
- Windows: `%LOCALAPPDATA%\WhittyArcade\roms` and
  `%LOCALAPPDATA%\WhittyArcade\chd`.

Put supported MAME ZIPs or extracted sets in `roms`, and disc media such as
`rrv1-a.chd` in `chd`. Files are read in place: WhittyArcade does not copy,
extract, modify or repack them. Open **ROM Folders** in the launcher to display
the active paths and run the installed-set audit.

The command-line audit and listing tools use the same folders:

```bash
./build/WhittyArcade --list-roms
./build/WhittyArcade --audit-roms
./build/WhittyArcade --audit-roms /path/to/game.zip
```

All three standard MAME layouts are supported:

- **Non-merged:** place the game ZIP in the ROM folder; it already contains
  its shared ROMs.
- **Split:** keep the listed parent or companion ZIP beside the game ZIP.
  Loaders search the selected archive first and then the exact companion.
- **Merged:** place the parent archive in the ROM folder. Entries in clone
  subdirectories are found by basename.

Required MAME short names for this build:

| Board | Games | Additional archives |
|---|---|---|
| Namco System 22 | `ridgerac`, `ridgera2`, `raverace`, `acedrive`, `victlap`, `cybrcomm`, `ridgeracf` (not working) | `namcoc71.zip`, `namcoc74.zip` |
| Namco Super System 22 | `timecris` (World, TS2 Ver.B), `dirtdash` (World, DT2 Ver.C), `aquajet` (World, AJ2 Ver.B) | `namcoc71.zip`; each set carries its M37710 firmware data |
| Namco System 246 | `rrvac` (Ridge Racer V: Arcade Battle) | `rrv1-a.chd` |
| Xbox 360 offline | `robotron` (Robotron: 2084) | Extracted `default.xex` with `classic/` and `media/` data |
| Sega Model 1 | `vformula`, `vf`, `swa`, `wingwar` | Split `vformula` also needs `vr.zip`; merged `vr.zip` works directly |
| Sega Model 2 | `srallyc`, `vcop2`, `vcop` | None (`segabill.zip` is optional for `srallyc`) |
| Phoenix hardware | `phoenix` | None |
| Galaxian hardware | `galaxian`, `mooncrst`, `uniwars` | None |
| Sega System 16B | `shinobi4` | Split collections also need `shinobi6.zip` for the unencrypted sound program; merged `shinobi.zip` works directly |
| Capcom Ghosts'n Goblins | `gng` | World parent set; ZIP or extracted directory, validated by exact MAME sizes and CRC-32 values |
| Namco Galaga | `galaga` | None |
| Namco System 1 | `pacmania` | None |

`vcop` (Virtua Cop, Revision B) is the original Sega Model 2 light-gun game.
Unlike the Model 2A sets it drives a Sega Model 1 sound board (68000 + YM3438 +
dual MultiPCM) and reads its guns and switches through a model1io2 cabinet-I/O
board (a TMPZ84C015 running the `epr-17181` firmware carried inside `vcop.zip`)
over dual-port RAM. Aim with the mouse, left click fires, right click reloads
(shoots off-screen); Player 2 shares the same controls. `ridgeracf` (Ridge
Racer Full Scale) is still detected and shown as not working. Clone revisions
not listed above are not silently substituted for the supported program
revision.

When a ROM path is supplied explicitly on the command line, WhittyArcade also
checks that path and its immediate directory for required parent, companion
and firmware archives. Automatic launcher discovery otherwise stays inside the
dedicated ROM and CHD folders.

Expected firmware:

- `c71.bin`: 8192 bytes, CRC `47c623ab`
- `c74.bin`: 16384 bytes, CRC `a3dce360`

The firmware may be loose or stored in `namcoc71.zip` and `namcoc74.zip`.
Time Crisis and Dirt Dash need `c71.bin` but use the M37710 program contained
in their own sets, so neither requires `namcoc74.zip`.

## WhittyArcade compared with FinalBurn Neo

WhittyArcade is not an FBNeo fork or replacement. The projects serve different
use cases:

| Area | WhittyArcade | FinalBurn Neo |
|---|---|---|
| Focus | Selected boards with explicit machine, cabinet, operator and frontend integration, including several Namco/Sega 3D systems | Broad arcade, console and computer coverage |
| Frontend | Purpose-built standalone Windows/Linux arcade launcher and pause menu | Standalone ports plus a widely used Libretro core |
| ROM workflow | Reads supported current-MAME ZIPs, extracted sets and CHDs directly from dedicated folders; no import/copy/repack step | Requires ROM sets matching the installed FBNeo version; official DAT/rebuild guidance is provided |
| Catalog | 26 working sets across 11 board/runtime groups | A substantially broader multi-system catalog |
| Cabinet tools | Per-game driving, twin-stick and light-gun paths; DIP/operator menus; EEPROM/NVRAM manager; verified high-score viewers | Broad input/DIP coverage, native cheats and optional `hiscore.dat` integration |
| Wider ecosystem | Focused desktop feature set; no netplay, rewind, run-ahead or achievements at present | The Libretro core supports saves, rewind, run-ahead, pre-emptive frames, netplay, achievements and cheats through RetroArch |

See the [official FBNeo repository](https://github.com/finalburnneo/FBNeo) and
[official Libretro FBNeo documentation](https://docs.libretro.com/library/fbneo/)
for its current scope and feature matrix. Standalone FBNeo and the Libretro
core do not expose exactly the same frontend features.

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
- W/A/S/D: Player 1 up/left/down/right; Z/X/C: Player 1 actions
- Arrow keys: twin-stick secondary directions
- I/J/K/L: Player 2 directions; U: Player 2 action 1
- `5`: coin 1; `6`: coin 2; `9`: service credit; `F2`: test switch
- `1`: start 1; `2`: start 2; Z/X: shift down/up; V/B/N/G: views 1-4
- `Esc`: return from gameplay to the arcade selector; from there, exit
- `P`: pause/resume every game; pausing also silences its audio and opens the
  shared in-game menu. That menu contains Play/Resume, DIP/operator settings,
  emulator settings, controls, game/ROM selection, and return to selector.
  Arrow keys or the controller D-pad/left stick navigate; Enter/A selects and
  Backspace/B returns to the pause menu.
- Letter keys, including `C`, `D`, `R`, and `S`, are normal mappable gameplay
  keys and are not bound to host menus
- Ctrl+Plus/Ctrl+Minus: resize the window from 1x to 4x; Alt+Enter toggles
  borderless fullscreen

Galaxian and UniWar S use their standard MAME `galaxian` and `uniwars`
archives. Press `5` to insert a coin and `1` to start. Player 1 uses A/D to
move and Z to fire; the cocktail Player 2 controls are J/L and U. These actions
can be reassigned from each game's **Controllers / Keyboard** page.

Time Crisis light-gun defaults:

- Move the mouse to aim; the normal pointer becomes WhittyArcade's cyan P1
  target sight. A hot red/magenta P2 sight is available to two-player gun
  profiles.
- Left click fires.
- The player begins hidden. Hold `Space` to press the cabinet pedal and stand;
  release `Space` to take cover and reload.
- Right click immediately forces cover/reload, even while the pedal action is
  held.
- `5` inserts a coin. Follow the cabinet prompts to begin play.
- Controller and keyboard alternatives remain editable under the per-game
  `timecris` profile. Moving a mapped aim axis switches from the mouse to that
  device; moving the mouse switches back.

Dirt Dash driving defaults:

- Left/Right steer; Up accelerates; Down brakes.
- Z/X shift down/up; V changes view; G is the motion-stop cabinet switch.
- `5` inserts a coin. Press the accelerator when the cabinet prompts for a
  decision/start input.
- Every action remains editable under the per-game `dirtdash` controller
  profile; standard and deluxe cabinet analog ranges are calibrated by the
  Super System 22 M37710 input path.

Aqua Jet cabinet defaults:

- Left/Right steer the handlebar; Up applies the throttle lever.
- The jet-ski cabinet's fore/aft body lean is mapped to the P1 up/down
  actions, so a controller stick or the mapped keys tilt the rider.
- There is no brake, gearshift or view button on this cabinet, so only the
  controls above appear in the `aquajet` controller profile.
- `5` inserts a coin and `1` starts a game.
- All three axes are driven backwards on the real board; the M37710 input
  path applies that inversion, so the host controls behave normally.

Settings are saved to
`~/.config/WhittyArcade/settings.ini` (or `$XDG_CONFIG_HOME`) on Linux and
`%LOCALAPPDATA%\WhittyArcade\settings.ini` on Windows. Existing
`ridge_racer_emulator` settings are imported automatically on Linux. Music
volume controls looping C352 voices; effects/speech volume controls one-shot
voices. Every board uses the same programme-loudness target and peak limiter;
the master volume is applied afterwards as the global listening level.
Input mappings are saved separately in
`~/.config/WhittyArcade/input.ini` (or `$XDG_CONFIG_HOME`) on Linux and the
same `%LOCALAPPDATA%\WhittyArcade` directory on Windows. They are reloaded for
each newly launched cabinet.

## EEPROM, NVRAM and high scores

The launcher's **EEPROM / NVRAM Manager** shows every persistent chip per game,
including its expected size and last modification time. It can create dated
backups, export a portable save folder, validate and restore an import, or
perform a recoverable factory reset. Existing data is backed up before restore
or reset.

Operator data lives below `$XDG_CONFIG_HOME/WhittyArcade/nvram` (normally
`~/.config/WhittyArcade/nvram`) on Linux or
`%LOCALAPPDATA%\WhittyArcade\nvram` on Windows:

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
`~/.config/WhittyArcade/hi`) on Linux or
`%LOCALAPPDATA%\WhittyArcade\hi` on Windows, and also detects existing files
in MAME's usual Linux `~/.mame/hi` and `~/.local/share/mame/hi` folders. A
score file is restored only after the game initializes its default table.
Other games remain clearly marked as not decoded; raw EEPROM bytes are never
presented as guessed scores.

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

Ace Driver, Victory Lap and Dirt Dash use the same steering controls, with
their cabinet-specific wheel and pedal ranges calibrated automatically. Cyber
Commando uses both controller sticks; on keyboard, W/A/S/D controls the left
stick and the arrow keys control the right stick. Z/X operate gun
trigger/missile and V changes view.

Time Crisis uses the mouse as an absolute light gun inside the 4:3 game area.
Its trigger and pedal still pass through the common action mapper, so another
gun cabinet can choose different buttons without changing the System 22 input
code.

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
- `src/system22_mcu.cpp` — C74/M37710 maps and C352 command producer
- `src/system22_sprites.cpp` — Super System 22 C374/VICS sprite decoding
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
- `src/gng_machine.cpp` — Capcom MC6809 board, graphics decode and compositor
- `src/gng_audio.cpp` — Ghosts'n Goblins Z80 and dual-YM2203 audio worker
- `src/gng_rom.cpp` — exact parent-set ROM validation and region assembly
- `third_party/model1` — standalone Model 1 geometry-board integration and
  Model 1/System 24 video
- `third_party/mb86233` — standalone Fujitsu geometry-processor core
- `third_party/musashi` — Musashi MC68020 core
- `third_party/m37710` — standalone M37702/M37710 CPU core
- `third_party/v60` — standalone NEC V60 CPU core
- `third_party/m6809f` — MIT-licensed instance-based MC6809 core
- `tests` — catalog/lifecycle, ROM, CPU/bus, audio, video and boot regressions

See [architecture](docs/architecture.md),
[adding games and boards](docs/adding_games_and_boards.md), and the
[cross-platform build scope](docs/cross_platform_build_scope.md) for extension
contracts, the required test path, and the Android-first release plan. The
separate [Google Play paid-distribution assessment](docs/google_play_sale_feasibility.md)
records the commercial licence, policy, privacy and store-release gates.
The ROM-backed [Namco System 246/256 plan](docs/system246_256_board_plan.md)
selects Ridge Racer V: Arcade Battle as the first target and records its
backend, media, licensing and validation gates.

## Credits and licensing

WhittyArcade's original code is proprietary software, Copyright © 2026
Jonathan Whittingham / CrownParkComputing. Official unmodified binaries are
licensed for personal, non-commercial use; commercial use and redistribution
require written permission. See [LICENSE](LICENSE) for the full terms.

System 22 behaviour is based on MAME's BSD-3-Clause `namcos22`, C352,
TMS320C2x and M37710 implementations by Phil Stroffolino, hap, R. Belmont,
superctr, Tony La Porta and other MAME contributors. The V60, Model 1 TGP and
video adaptations retain their BSD-3-Clause notices and Olivier Galibert's
copyright. Musashi is by Karl Stenerud under its permissive license. Source
files retain their applicable copyright and license notices.

WhittyArcade does not include ROMs, firmware, keys, or copyrighted game data.
Third-party components retain their own licences and attributions; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
