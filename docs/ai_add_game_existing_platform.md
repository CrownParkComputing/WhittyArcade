# AI handoff: add a game to an existing MANX platform

> **Note:** This is the MANX rebrand of the MANX handoff document.
Project name in code is still `MANX` until Phase 2.
Give this document to the AI that will implement the next game. Fill in the
task inputs first. The instructions deliberately exclude new hardware
platforms.

## Task inputs

- Game display name: `<GAME_DISPLAY_NAME>`
- Stable MAME short name: `<MAME_SHORT_NAME>`
- Existing MANX platform: `<EXISTING_PLATFORM>`
- Legal local ROM archive or directory: `<LEGAL_ROM_PATH>`
- Optional parent/device archives: `<COMPANION_ROM_PATHS>`
- Optional read-only reference project: `<REFERENCE_PATH>`
- Commit and push authorised: `<YES_OR_NO>`

## Assignment

Add `<GAME_DISPLAY_NAME>` to the existing `<EXISTING_PLATFORM>` implementation
in MANX. This must be a native extension of the current C++/CMake
emulator. Reuse the platform's existing CPU, bus, video, audio, input, session,
launcher and ROM-library paths.

Do not create a new platform. In particular, do not add an
`arcade_board_type`, board descriptor, standalone frontend, parallel emulator,
Android project, Gradle build, Kotlin source, or game-specific application
entry point. `arcade_board_count` must remain unchanged.

If inspection proves that the game does not run on one of the platforms
already represented by `arcade_board_type`, stop and report the exact missing
hardware. Do not disguise a new board as an existing one and do not ship a
non-emulating placeholder.

## Existing platform boundaries

The currently registered platform families are defined in
`include/arcade_types.h` and `src/arcade_catalog.cpp`:

| Platform family | Existing board type | Primary implementation |
| --- | --- | --- |
| Namco System 22 / Super System 22 | `system22` | `system22_rom.cpp`, `system22_session.cpp`, `system22_*` modules |
| Sega Model 1 | `model1` | `model1_rom.cpp`, `model1_session.cpp`, `model1_*` modules |
| Sega Model 2 | `model2` | `model2_rom.cpp`, `model2_session.cpp`, `model2_*` modules |
| Phoenix hardware | `phoenix` | `galaxian_rom.cpp`, `galaxian_session.cpp`, Phoenix board/audio profiles |
| Galaxian hardware | `mooncrst` | `galaxian_rom.cpp`, `galaxian_session.cpp`, Galaxian board/audio profiles |
| Sega System 16B | `shinobi` | `shinobi_rom.cpp`, `shinobi_session.cpp`, `shinobi_*` modules |

A verified per-game configuration or board revision may be added inside the
owning platform module. It must not leak into `main.cpp`, launcher widgets, ROM
menus, or unrelated platforms.

## Non-negotiable constraints

1. Work only in the MANX repository. Treat any ZArcade, MAME, or other
   project as read-only reference material.
2. Use native C++ and the existing CMake build. Do not introduce Gradle,
   Kotlin, Java, an Android module, a second GUI, or a second build system.
3. Never copy, move, generate, embed, stage, commit, upload, or publish ROMs,
   firmware, CHDs, keys, save data, screenshots containing extracted assets,
   or local file paths.
4. ROM paths belong only in the local CMake cache or command line. All ROM
   names stored in source must be the required archive entry names, not host
   paths.
5. Preserve the proprietary `LICENSE`, copyright notices, and
   `THIRD_PARTY_NOTICES.md`. Do not paste implementation code from MAME or
   another project into this proprietary codebase. Derive hardware facts and
   write an independent implementation. Import third-party code only when its
   licence is compatible, its use is necessary, and its notice is recorded.
6. Preserve unrelated user changes. Inspect `git status` before editing and do
   not reset, overwrite, or reformat unrelated files.
7. Do not claim that a game works because it reaches a menu or avoids a crash.
   CPU execution, coin/start, controls, video and audio must be meaningfully
   exercised.
8. Do not guess protection, memory maps, clocks, ROM interleaving, palette
   formats, input polarity, DIP meanings or high-score addresses. Verify them
   from reliable technical references and observed behaviour.
9. Do not commit or push unless the task input explicitly authorises it.

## Required implementation workflow

### 1. Establish a clean baseline

Read `docs/architecture.md` and `docs/adding_games_and_boards.md` completely.
Inspect the owning platform's ROM loader, machine profile, session, input map,
audio/video path and existing tests before editing.

Record the initial worktree state and run the relevant existing tests. Any
pre-existing failure must be distinguished from a regression caused by this
task.

### 2. Verify the ROM set and platform match

Inspect the legal local archive by entry name, size and checksum without
extracting it into the repository. When MAME is available, verify the set with
a command equivalent to:

```bash
mame -rompath /path/to/rom-directory -verifyroms <MAME_SHORT_NAME>
```

Establish all of the following before implementation:

- exact game/set identity and parent relationship;
- CPU and sound devices;
- memory and I/O maps;
- ROM regions, widths, byte order, interleave and transforms;
- clocks, refresh rate, visible area and screen orientation;
- video layers, palettes, sprites and any priority rules;
- audio devices and command paths;
- input polarity, cabinet controls, service inputs and DIP defaults;
- protection, banking, NVRAM/EEPROM and companion archive requirements.

Compare those facts with the selected existing platform. If a fundamental
device or architecture is absent, stop under the no-new-platform rule.

### 3. Extend the existing ROM loader

Use the owning `*_rom.h/.cpp` files. Add the smallest platform-local set enum
or declarative profile needed, then implement:

- content-based identification using multiple distinctive entries;
- the stable MAME short name and accurate display name;
- strict required-entry and exact-size validation;
- correct region assembly, interleave, endianness and verified transforms;
- precise errors naming a missing or invalid entry;
- merged, split and non-merged handling consistent with the manifest.

Filename aliases may assist collection scanning, but identification must be
based on archive contents. Unknown or incomplete sets must fail closed.

### 4. Register exactly one catalog manifest

Add one `rom_set_manifest` in `src/arcade_catalog.cpp`, assigned to the
existing `arcade_board_type`. Update `arcade_game_count` to match the manifest
array, but do not change `arcade_board_count`.

Declare the split parent and extra device archives accurately. Mark `working`
as `true` only after the acceptance checks pass. The existing catalog must
drive the launcher, ROM library, controller page and persistent-data tools;
do not add duplicate game lists elsewhere.

### 5. Add only verified per-game machine configuration

Reuse the existing platform machine and session. Add a platform-local game
profile only for real differences such as ROM banking, protection keys,
cabinet I/O, palette wiring, sound-board selection or a documented map
variant.

Keep application-facing interfaces board-neutral. Do not branch on the game
name in `main.cpp`. Route the detected set to its profile in the existing
platform session and ensure switching ROMs constructs fresh game state.

### 6. Wire controls through the shared mapper

Expose only controls the cabinet actually has. Reuse existing neutral
`input_state` actions where possible. Add clear labels and sensible keyboard,
controller or mouse defaults only when the existing mapping system requires
them.

Coin, Player 1 start, Player 2 start, service and test must retain their common
frontend behaviour. All playable actions must remain remappable through
**Controllers / Keyboard**; do not hard-code a separate in-game input path.

### 7. Reuse video and audio paths

Use the existing platform renderer and audio worker. Implement only verified
game-specific differences. Preserve frame timing, orientation, aspect ratio,
audio pause/resume, volume controls and clean shutdown during ROM switching.

Do not substitute screenshots, prerecorded audio, scripted attract sequences
or other presentation-only approximations for emulation.

### 8. Add tests at the appropriate levels

At minimum, update or add:

- catalog registration and unique content-probe coverage;
- ROM-loader tests for identification, complete loading and rejection of
  missing/incorrect entries;
- a no-ROM machine contract test for any new map, bank, input or sound wiring;
- an optional legal-ROM boot regression driven by a CMake cache path;
- coin/start and representative gameplay input assertions;
- meaningful changing-frame, palette/sprite/layer and audible-output checks;
- repeated session construction/destruction coverage when lifecycle wiring
  changes.

The no-ROM test path must remain runnable in GitHub CI. A local ROM-backed test
must be enabled only when its cache variable is supplied, for example:

```cmake
set(<PLATFORM>_<GAME>_TEST_ROM "" CACHE FILEPATH
    "Optional legal local ROM used by the <game> integration test")
```

Never weaken an existing assertion merely to make the new game pass.

### 9. Document user-facing behaviour

Update the supported-games list, ROM requirements and controls in `README.md`
and `RELEASE_README.md` where appropriate. State parent/device archive needs
and known emulation limitations honestly. Repeat that users must supply ROMs
they are legally entitled to use.

Do not add ROM download links, game artwork taken from ROMs, publisher logos
or claims of affiliation.

## Verification and acceptance

Configure the ROM path locally, build the affected targets, then run the full
suite. Adapt cache-variable names to the owning platform:

```bash
cmake -S . -B build -D<PLATFORM>_<GAME>_TEST_ROM=/legal/local/game.zip
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/MANX --audit-roms /legal/local/game.zip
```

Also launch the game through the normal GUI, insert a coin, start a game and
exercise every gameplay control. Compare timing, orientation, palette,
starfield/backgrounds, sprites, priorities and audio with a trusted reference
when practical.

Before delivery, run:

```bash
git diff --check
git status --short
```

Inspect the complete diff and confirm all of the following:

- no ROM, firmware, CHD, key, save, build output or generated binary is staged;
- no absolute local path, account data or credential is present;
- no Gradle, Kotlin, Java or Android project file was introduced;
- no new board type, board descriptor or application entry point was added;
- the full non-ROM suite passes and all available legal-ROM tests pass;
- documentation and test counts match the actual catalog.

## Completion report

Return a concise evidence-based report containing:

1. the game and exact ROM set now supported;
2. the existing platform/profile reused;
3. machine, input, video and audio differences implemented;
4. ROM dependencies and controls;
5. build, test, ROM-audit and live-play results;
6. any remaining limitations;
7. changed file summary;
8. commit hash, push result and CI/artifact status only when authorised.

If the game cannot be completed without adding a new platform, report that as
the blocker and leave the repository unchanged apart from any explicitly
requested research notes.
