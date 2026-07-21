# Adding games and hardware boards

This is the required extension path. Keep hardware facts in the owning board
module and keep application code board-neutral.

## Add a game to an existing board

1. Extend that board's ROM-set enum and declarative asset description in its
   `*_rom.h/.cpp` files. Implement content-based identification, the stable
   MAME `set_short_name()`, display name and validated region loading.
2. Add one `rom_set_manifest` to `arcade_catalog.cpp`. Set its board, split
   parent, extra archives and working status accurately. Do not add the name
   to the launcher, EEPROM page or high-score page; those are catalog-driven.
3. If the set uses a shared device/parent archive not already described, add
   it once to `companion_archives` in `rom_library.cpp`. Filename aliases are
   scan hints only; loader content probing remains authoritative.
4. Add only genuinely game-specific machine configuration to the owning
   board module (protection key, cabinet profile, memory-map variant, and so
   on). Do not branch in `main.cpp`.
5. Persistent operator storage appears automatically for existing System 22,
   Model 1 and Model 2 layouts. A high-score decoder is optional and must be
   based on verified addresses/encoding/checksums; otherwise the UI reports
   that the table is not decoded.
6. Add a loader contract test and, when a legal local ROM path is supplied,
   a full ROM/boot regression. The catalog test must continue to prove that
   every loader short name resolves uniquely.

### Worked example: Time Crisis

Time Crisis demonstrates both the short extension path and how board variants
stay contained. Its launcher support is one catalog manifest plus a declarative
`timecris` ROM profile. The frontend, ROM browser, audit, persistent-data page
and controller page discover it automatically.

The game also exposed real Super System 22 differences, so the owning board
module gained a variant flag, the later 24-bit bus layout, M37710 boot path,
C374/VICS sprites, mixer/fade handling and light-gun registers. Mouse aim,
trigger and pedal are translated into the existing neutral `input_state`; no
Time Crisis branch was added to the launcher or application entry point.

For another game on an already covered hardware revision, only the ROM
profile, manifest, genuine protection/cabinet differences and tests should be
needed. A newly discovered board revision should extend the board module as
Time Crisis did, rather than leaking special cases into the application shell.

## Add a new hardware board

1. Add one value to `arcade_board_type` and one descriptor to the board table
   in `arcade_catalog.cpp`. Update `arcade_board_count`.
2. Add the board ROM loader and its first game manifest, then register its
   content probe in `arcade_game_probe.cpp`.
3. Implement machine state in board-named modules. CPU buses, audio devices,
   geometry/video and persistent chips belong here; avoid process-global
   state so repeated ROM switches are safe.
4. Add `<board>_session.cpp`, deriving from `video_emulator_session`. It owns
   board input/audio wiring, operator settings and frame timing. Export one
   `make_<board>_session()` through `arcade_session_internal.h`.
5. Add one branch to the central factory in `arcade_session.cpp` and list the
   new source modules in CMake. No application-shell or launcher branch should
   be necessary.
6. Add tests at four levels:
   - metadata uniqueness and probe routing;
   - ROM region/interleave validation;
   - CPU/bus/audio/video unit tests;
   - boot/frame and repeated session-lifecycle integration.

## Local ROM-backed test configuration

ROM paths stay in the local CMake cache and are never embedded in source:

```bash
cmake -S . -B build \
  -DMODEL1_TEST_ROM=/path/to/vformula.zip \
  -DMODEL2_TEST_ROM=/path/to/srallyc.zip \
  -DSHINOBI_TEST_ROM=/path/to/shinobi.zip \
  '-DSYSTEM22_TEST_ROMS=/path/to/ridgerac.zip;/path/to/raverace.zip;/path/to/timecris.zip'
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `GALAXIAN_PHOENIX_TEST_ROM`, `GALAXIAN_BASE_TEST_ROM`,
`GALAXIAN_MOONCRST_TEST_ROM`, `GALAXIAN_UNIWARS_TEST_ROM`,
`MODEL1_VF_TEST_ROM`, `MODEL1_SWA_TEST_ROM` and `MODEL1_WINGWAR_TEST_ROM` for
the other existing integrations.

## Completion checklist

- A missing/incorrect ROM fails with a precise loader error.
- Split, merged and non-merged archive layouts follow the manifest contract.
- Coin, start, service and operator settings work through common host input.
- Session objects release process-owned output across at least 25 repeated
  cross-board construction boundaries; live audio/video shutdown has
  board-specific integration coverage.
- The full non-ROM suite and every available ROM-backed test pass.
- User documentation lists the set and any remaining hardware limitations.
