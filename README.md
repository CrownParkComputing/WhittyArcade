# ![MANX logo](logo-mark.svg) MANX — Multiscreen Arcade Nexus

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platforms: Windows · Linux](https://img.shields.io/badge/platforms-Windows%20·%20Linux-blueviolet.svg)]()
[![Working sets: 26](https://img.shields.io/badge/working%20sets-26-success.svg)]()

**MANX** is the rebrand of the WhittyArcade project. It is a focused standalone
multi-board arcade emulator for Windows and Linux. Its 26 working sets span
Namco System 22 / Super System 22, System 246, Model 1, Model 2, Namco
System 1, Galaga, Galaxian, Phoenix, Sega System 16B, Ghosts'n Goblins
hardware and the Xbox 360.

MAME's BSD hardware implementations are the primary behaviour reference;
machine integration, scheduling, frontend, storage and host output are
standalone.

![MANX wordmark](logo-wordmark.svg)

## Why the rebrand

The original name was WhittyArcade. MANX keeps the codebase, the working
sets and the existing plug-in host, and gives the project a name that puts
its distinguishing feature — **multiscreen cabinet support** — at the
front. The Multiscreen Arcade Nexus is the goal: one launcher that knows
about cabinet form, screen count and link topology, and shows games
through that lens instead of a flat list.

## Multiscreen first

Every working set in MANX has been tagged with the cabinet form it actually
needs:

- **Single-screen uprights** — Galaga, Galaxian, Phoenix, Pac-Mania, Shinobi
  twin-stick, Ghosts'n Goblins, and the 2D classics.
- **Sit-down / single-cockpit** — Ridge Racer, Virtua Formula, Star Wars
  Arcade, Time Crisis (gun), Dirt Dash World DT2, System 22 / Super
  System 22 main boards.
- **Multi-cabinet / linked** — Galaga twin-cockpit, Shinobi twin-cockpit,
  the System 22 / Super System 22 link fabric and the Model 2 link board.
- **Multi-monitor wall** — System 246 boards driven as a 3-column wall,
  and Burnout 3 / Geometry Wars as native plugins that take the same
  launcher shim.

The next milestone of the launcher is to make these groups visible in the
menu: a single-screen tab and a multi-screen tab, each with a 2D carousel
of cabinet previews and the per-game cabinet metadata underneath.

## Current status

Implemented and tested (most relevant items; full list in `docs/`):

- Motorola MC68020 execution through the standalone Musashi 4.60 core
- Big-endian System 22 and Super System 22 main memory maps
- System 22 interrupt controller and game-programmable vblank IRQ delivery
- Directory and ZIP ROM loading with MAME sizes and CRC-32 validation
- Per-game program interleave, ROM-region placement and CRC validation
- Two real TMS320C25-derived C71 cores with master/slave program and
  polygon-RAM maps
- 32-voice C352 mixing with C74-to-C352 register traffic
- OpenGL 4.3 renderer with thread-safe polygon producer/consumer boundary
- C71 direct-render command decoding and hardware triangle rendering
- Ridge Racer's linked PDP command stream, point-ROM/point-RAM object lists
  and live palette RAM
- Super System 22 C374/VICS sprite-list decoding
- Time Crisis gun coordinates, cabinet trigger/pedal, mouse aiming and
  per-player crosshairs
- Hot-pluggable SDL / Xbox and keyboard controls with per-device launcher
  mapping and calibrated axes
- System 24 RAM tile pages, character decoding and 3D composition for
  Model 1 (Virtua Formula, Virtua Fighter, Star Wars Arcade, Wing War)
- System 246 main board path with PCSX2 reimplementation in progress
  (`system246-pcsx2-rewrite` branch)
- Geometry Wars and Burnout 3 standalones as plugins hosted by the same
  launcher shim

## Building

### Linux

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

See [LINUX_README.md](LINUX_README.md) for distro-specific dependencies
and packaging notes.

### Windows

See [WINDOWS_README.md](WINDOWS_README.md). MSVC + vcpkg + Vulkan SDK.

## Project layout

```
src/             emulator and runtime
include/         shared headers
docs/            architectural notes, board plans, marketing collateral
scripts/         build helpers
game_data/       runtime metadata
roms/            placeholder for the ROM set (not in git)
third_party/     upstream code (Musashi, moira, ...)
tests/           unit and integration tests
logo-*.svg       brand assets
whittyarcade.svg LEGACY — kept during the rebrand transition; do not use
CMakeLists.txt   project(WhittyArcade) — renamed in the next release
```

## License

GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for upstream licenses.

## Brand

- Mark (square app icon): `logo-mark.svg`, also as `logo-512.png` /
  `logo-256.png` / `logo-128.png` / `logo-64.png`.
- Wordmark (banner): `logo-wordmark.svg` / `logo-wordmark.png`.
- Emblem (round badge): `logo-emblem.svg` / `logo-emblem-256.png`.

When linking back to the project, please use the mark and the wordmark
together; the emblem is for icon-only contexts.

## Contributing

See [docs/architecture.md](docs/architecture.md) for the emulator
internals and [docs/cross_platform_build_scope.md](docs/cross_platform_build_scope.md)
for the platform matrix.

## Migration notes

The rebrand is staged:

- **Phase 1 (this commit)**: marketing identity, logo, README, top-level
  docs, code-comment banners. Identifiers in source still say
  `WhittyArcade`. Build is `project(WhittyArcade)`.
- **Phase 2**: identifier rename, project-name change, single-screen /
  multi-screen launcher tabs, animated 2D carousel.
- **Phase 3**: GitHub-side repo rename to `CrownParkComputing/MANX`,
  redirect from `WhittyArcade`, release tagging.
