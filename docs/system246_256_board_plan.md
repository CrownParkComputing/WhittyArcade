# Namco System 246/256 board plan

Status: research and ROM-backed feasibility validation completed on 2026-07-22.
The first target is **Ridge Racer V: Arcade Battle (RRV3 Ver.A)**, using the
stable MAME short name `rrvac`.

---

## MAJOR PIVOT (2026-07-24): replace Play! with an embedded PCSX2X6 core

**Decision by the project owner (Jon):** scrap the Play! backend entirely — no
Play! code carried forward, no more patching — and build the System 246 core
on the **PCSX2X6** spin-off of PCSX2, booting the **real System 246 BIOS**
(`r27v1602f.7d`, user-supplied) so the genuine firmware + driver chain runs
exactly as on hardware ("do what PCSX2 does"). This supersedes the "Play! as
base, PCSX2X6 as comparison only" decision recorded in the `## Decision`
section below.

**Why:** Play! is a high-level-emulation core. Its EE "BIOS" is hand-assembled
in C++ (`CPS2OS` + `CMIPSAssembler`); it never executes a real BIOS, so it can
never "do what PCSX2 does." After extensive work its RRV streamed-audio stayed
broken (engine never dynamically revves; see the 2026-07-23/24 findings later
in this doc). PCSX2X6 is a full low-level PS2 emulator that boots the real BIOS
and lists RRV playable — the correct architecture for this board.

**Licensing consequence — decided:** PCSX2X6 is **GPLv3**; WhittyArcade was
**proprietary** ("all rights reserved"). They cannot coexist in one binary
unless the whole program is GPLv3. **The owner chose to embed the PCSX2 core
directly and relicense WhittyArcade to GPLv3.** The proprietary licence is
being given up for this. (The alternative — running PCSX2X6 as a separate
process to keep WhittyArcade proprietary — was considered and rejected in
favour of tighter integration.) *Do not actually change `LICENSE` until the
proof-of-concept gate below passes and real PCSX2-derived code starts landing,
so the licence is not surrendered for a dead end.*

**Hard constraint — x86-64 only:** PCSX2's EE/IOP/VU recompilers have no ARM
backend. WhittyArcade's `CMakeLists.txt` has aarch64/arm branches; on those
targets the System 246 board will be unavailable (build it out with a guard).
Desktop x86-64 (the owner's CachyOS box) is fine.

**Play! removal footprint (clean):** `third_party/play` (87 MB submodule),
`src/system246/play_*.{cpp,h}` (~16 files), and only ~5 WhittyArcade source
files reference it. `atapi_registers.*`, `rrv_music.*`, `system246_rom.*`,
input/video bridge glue and the ROM/media cache may be reusable; the Play!
VM wiring (`play_core.*`) is discarded.

### Phased plan

0. **Proof-of-concept gate (IN PROGRESS).** Build PCSX2X6 natively on this
   machine and have the owner confirm it plays RRV correctly *with real audio*
   using their BIOS + `rrvac.zip` dongle + `rrv1-a.chd`. **No embedding or
   relicensing happens until this passes** — if PCSX2X6 itself doesn't nail
   RRV, the whole plan is void. (Configured 2026-07-24, Release/no-LTO,
   clang+lld, all system deps present: Qt 6.11.1, SDL2, Vulkan 1.4, zstd,
   libpcap. `pcsx2-qt` build launched.)
1. Relicense WhittyArcade to GPLv3 (`LICENSE` + headers + README).
2. Remove Play!: drop the submodule and `play_*` sources; keep the reusable
   glue identified above behind the existing board abstraction.
3. Bring the PCSX2 core into the WhittyArcade build (link `pcsx2` + `common` +
   `3rdparty` static libs; PCSX2 is not a library by design, so this follows
   the `pcsx2-gsrunner` headless pattern, not the Qt app).
4. Implement PCSX2's `Host` interface against WhittyArcade services: GS video
   → WhittyArcade renderer/window, SPU2 audio → WhittyArcade audio output,
   input → WhittyArcade input channel, settings → hardcoded System 246 profile.
5. Drive `VMManager` to boot RRV (BIOS + dongle + disc) and wire it into the
   System 246 board entry point.
6. Test + polish; re-run the cross-board teardown soak before shipping.

### OWNER DECISION (2026-07-24): full rewrite from PCSX2 regardless

The owner reviewed the "PCSX2X6 has the same bug" finding and **still chose a
complete rewrite**: remove Play! and the entire System 246 board "as if it was
never added," and rebuild from PCSX2. Rationale accepted: PCSX2 produced real
dynamic engine audio (corrupt "vroom-vroom", a timing race — not silence/
static like Play!), has active upstream development, and ships dedicated RRV
arcade code, so it is a better base on which to eventually fix the streaming
race. WhittyArcade is being relicensed to GPLv3 to permit embedding the PCSX2
core (owner-approved).

**Progress (branch `system246-pcsx2-rewrite`, `main` untouched):**
- **Phase 1 removal — DONE & verified.** `third_party/play` submodule,
  `src/system246/`, `src/system246_*.{cpp,h}`, all `tests/system246_*`, and
  every CMake/enum/catalog/session/probe/persistent-data/rom-library reference
  removed. `grep -rniE "system246|play_core|PlayCore|rrvac|WHITTY_PLAY"` over
  src/include/tests/CMakeLists is CLEAN. Build green (62 targets), ctest 37/37.
  (Also fixed the stale `srallyc`-only system-link assertion — now accepts
  `ridgera2`/`raverace`/`srallyc`.)
- **PCSX2X6 relocated** from ephemeral tmpfs to `/home/jon/pcsx2x6` (source +
  completed clang/Release build; BIOS + RRV `.acgame` staged under
  `build/bin/`). Pinned tag v0.2.14, commit 88d490bb, remote
  github.com/PS2Homebrew-arcade/pcsx2x6.
- **Link recipe established** (no symbol conflicts — WhittyArcade uses system
  SDL3/zlib/zstd; PCSX2 bundles its own fmt/imgui/cubeb which WhittyArcade
  doesn't use). 18 static libs from `/home/jon/pcsx2x6/build/`
  (`pcsx2/libpcsx2.a`, `common/libcommon.a`, + 16 `3rdparty/*`), wrapped in
  `--start-group/--end-group`, plus system libs: `dl backtrace curl dbus-1
  fontconfig freetype GLX jpeg lz4 OpenGL pcap png udev webp X11 Xext Xi
  xkbcommon Xrandr Xrender z zstd pthread SDL3` (+ ryml/plutosvg/plutovg).
  `VMManager::{Initialize,Execute}` and `VMManager::Internal::CPUThread*` are
  exported. Reference frontend for the `Host` interface + init sequence:
  `/home/jon/pcsx2x6/pcsx2-gsrunner/Main.cpp`.
- **Phase 3 (link foundation) — DONE & RUNS.** `src/system246/pcsx2_host.cpp`
  provides the 55 frontend `Host::`/`InputManager::` symbols `libpcsx2.a`
  needs (ported from gsrunner). `system246_pcsx2_link_test` target (clang++
  custom command, ABI-matched to the clang-built libs, x86-64-guarded, kept
  out of the default ALL build so a PCSX2 link error can't break the tree)
  **links green AND runs headless to `CPU thread init OK`/`shutdown OK`** —
  VM memory, x86 JIT recompiler, cpuinfo, settings all init with no display.
- **Relicensed to GPLv3 — DONE.** `LICENSE` replaced with the full GPL v3
  text (was the WhittyArcade proprietary licence). Follow-up sweep still
  needed: README + per-file copyright/licence headers + a NOTICE.
- **Phase 4 (boot RRV harness) — BUILT & GREEN.**
  `src/system246/pcsx2_boot_rrv.cpp` + target `system246_pcsx2_boot_rrv`:
  creates an SDL3+Vulkan 1280x720 window, hands its native (X11/Wayland)
  handle to PCSX2's GS via `Host::AcquireRenderWindow`, runs the VM on a
  dedicated CPU thread (`CPUThreadInitialize`→`Initialize`→`Execute`), enables
  **cubeb** audio, and boots RRV via `VMBootParameters.filename =
  .../rrvac.acgame` (GS renderer=Vulkan, framelimit on). `pcsx2_host.cpp`
  extended with a real window hook + a working `RunOnCPUThread` queue +
  `PumpMessagesOnCPUThread`. Links green (29 MB). Owner runs
  `./build/system246_pcsx2_boot_rrv`. **Known gap: input not yet wired** (game
  boots + renders + attract audio, but no controls — deliberately deferred to
  avoid two threads pumping SDL; JVS routing is the next task).
- **Phase 5 (input) — BUILT & GREEN.** `src/system246/pcsx2_input.h` +
  implementation in `pcsx2_host.cpp`: keyboard captured on the main SDL thread
  into atomics; `WhittyArcadeInput::ApplyToJVS()` pushes held state + queued
  coins into `ACJV::{SetWheelAxis,SetButtonState,InsertCoin}` on the CPU thread
  from `Host::PumpMessagesOnCPUThread` (verified called each vsync by
  `PollInputOnCPUThread`, before `PollSources`; ACJV state is level so it
  persists — PCSX2's own SDL input source stays disabled, no cross-thread SDL
  pumping). Map: arrows/WASD = steer/gas/brake, E/Q = gear up/down, V = view,
  Enter/1 = START, 9 = SERVICE, 5 = coin, Esc = quit. Both PCSX2 targets build
  green. Owner runs to confirm controls + finally judge engine audio in a race.
  (Gear/view JVS bits are a first guess — refine after the owner drives.)
- **Phase 6 (GUI integration) — BUILT & GREEN.** System 246 is a real
  WhittyArcade board again: catalog lists "Namco System 246" → "Ridge Racer V:
  Arcade Battle (rrvac)"; selecting it creates a PCSX2-backed session that
  boots RRV surfaceless in-process and presents into WhittyArcade's own window.
  Whole tree builds green under clang, PCSX2 linked into the main binary (40 MB,
  130 VMManager symbols), ctest 37/37. New `system246_pcsx2_session.cpp` +
  `system246_pcsx2` OBJECT lib (isolates C++20/-fno-exceptions/PCSX2 defines
  from the C++17 tree) + the 18 static libs. Frame capture via
  `GSSaveSnapshotToMemory` in `Host::BeginPresentFrame` → double-buffer →
  `present_rgba_frame` in `run_frame`; `producer_paced()`=true. NOT
  runtime-verified (owner tests). Known follow-ups: inputs are digital
  on/off (analog steering needs an analog path in the ACJV bridge);
  session ignores `rom_path` and boots the fixed pcsx2x6 `.acgame` while the
  ROM-library readiness still gates on `rrv1-a.chd`+`rrv3vera.ic002` (entry may
  show "not ready"); frame aspect/row-order unverified (may need a flip);
  per-frame snapshot alloc is a later optimization.
  Original design (all pieces de-risked):
  - **clang everywhere.** WhittyArcade builds clean with clang (2 trivial
    model1 brace-narrowing fixes applied), so the clang-built PCSX2 libs link
    DIRECTLY into the main binary — no C-ABI shim. (Switch the default
    compiler to clang.)
  - **Render bridge = `present_rgba_frame`**, same as the Xbox 360 board
    (`src/xbox360_session.cpp` is the template — a `video_emulator_session`
    whose `run_frame()` runs the emu, grabs an RGBA frame, calls
    `m_gpu_renderer->present_rgba_frame`). PCSX2 runs **surfaceless**; frames
    captured via `GSSaveSnapshotToMemory(...)` (pcsx2/GS/GS.h → RGBA
    `std::vector<u32>`) in `Host::BeginPresentFrame`, double-buffered to the
    session. No Vulkan/OpenGL window conflict (WhittyArcade keeps its GL
    presenter; PCSX2 renders offscreen).
  - **Input** already done — the session maps WhittyArcade input to
    `WhittyArcadeInput::` setters → ACJV/JVS.
  - Re-add board wiring: enum + catalog + `system246_rom`/`system246_controls`
    (restored from `main`, backend-independent) + session dispatch; new
    `src/system246/system246_pcsx2_session.cpp` = `make_system246_session`.
  - x86-64-guarded; ARM gets a stub session.
- **Phase 7 — LATER.** 60fps pacing polish, bezel compositing, and the
  streamed-engine audio-race work — once RRV drives inside the GUI.

**PCSX2 embedding blueprint (phases 3–5), from `pcsx2-gsrunner` + `pcsx2-qt`:**
- Link `libpcsx2.a` + `common` + PCSX2's `3rdparty` into WhittyArcade (static).
  PCSX2 core is x86-64 only — guard the whole board out on aarch64/arm.
- Drive the VM on a dedicated CPU thread:
  `VMManager::Internal::CPUThreadInitialize()` →
  `VMManager::Internal::LoadStartupSettings()` →
  `VMManager::Initialize(VMBootParameters{...})` →
  `VMManager::SetState(VMState::Running)` → `VMManager::Execute()` (blocks) →
  `VMManager::Shutdown(false)` → `VMManager::Internal::CPUThreadShutdown()`.
- Implement PCSX2's `Host::` interface (~50 methods; most are trivial stubs —
  achievements/clipboard/text-input/etc. return default). Load-bearing ones:
  `Host::AcquireRenderWindow`/`ReleaseRenderWindow` (hand PCSX2's Vulkan GS a
  render surface — reuse WhittyArcade's existing Vulkan window/`WindowInfo`),
  `Host::BeginPresentFrame`, `Host::OnGameChanged`, and the settings hooks.
- Audio: gsrunner sets `SPU2/Output OutputModule=nullout`; WhittyArcade needs a
  real `AudioStream` backend feeding `arcade_audio_output` (either PCSX2's
  cubeb/SDL backend or a thin custom `AudioStream` bridging to our mixer).
- Boot RRV as an arcade `.acgame`-equivalent: `VMBootParameters` with the
  dongle as `mc0:`, disc via ACATA with **`media = HDD`** (CONFIRMED type),
  gameid `NM00001`. Reuse the arcade boot flow in `VMManager.cpp`
  (`isArcadeManifest`/the `[data]` parser) as the reference.
- Only one PCSX2 instance per process (global VM state) — fine for one System
  246 game at a time.

### PIVOT REVISED — the SAME bug is in PCSX2X6 too (2026-07-24)

Built PCSX2X6 natively (proof-of-concept gate), staged the real BIOS + dongle
+ disc, and had the owner run RRV. Findings:

- **The disc is a HARD DISK image**, definitively: `rrv1-a.chd` CHD header has
  `unitbytes=512` + `GDDD` hard-disk metadata (not CD/DVD). RRV reads it over
  **ATA**, matching the dongle's `acata`/`ATA_driver` modules. WhittyArcade
  detects the `GDDD` tag correctly but then *extracts it to an "ISO" and feeds
  Play! optical CD/DVD media* — a media-type mismatch. Fixed the PCSX2X6
  manifest to `media = HDD` (was CD).
- **PCSX2X6 reproduces the exact bug anyway.** With correct HDD media, engine
  audio is fine through menus/loading, then at **race start** it corrupts —
  "bang, then random," once "very corrupt but vroom-vroom." Intermittent
  (worked once), i.e. **timing-dependent**.
- PCSX2X6 has *dedicated* RRV streaming code (V257 drive-board UART responder,
  FCA-1 I/O, and an explicit "avoid a race where SPU2 DMA completes before
  disc data is ready" fix in `ACATAPI.cpp`) and *still* doesn't fully nail it.
  Note that race-fix lives in the **ATAPI** path; HDD media uses **ACATA**,
  which may lack the equivalent — a possible reason HDD gives corrupt
  vroom-vroom rather than clean.

**Conclusion:** the RRV engine-audio bug is a **real-time HDD→SPU2 streaming
timing race**, present in BOTH Play! and the mature reference emulator. It is
**not** a Play! architecture limitation, not the media type alone, and not a
setup error. Therefore **embedding PCSX2X6 does not fix it** — the whole
"scrap Play!, embed PCSX2, relicense to GPLv3" pivot buys nothing for this bug
and is **abandoned**. `LICENSE` was never touched and Play! was never removed,
so there is nothing to revert. The realistic options are (a) tune emulator
timing/SPU2-sync settings to stabilise the stream, (b) ship RRV with the one
streamed engine channel muted/replaced (clean instead of corrupt), or (c) a
deep timing/DMA-pacing fix in whichever core we keep — low odds given even
PCSX2X6's dedicated code doesn't fully solve it. PCSX2X6 stays as an external
reference only (its original role), not a dependency.

---

## Decision (SUPERSEDED — see MAJOR PIVOT above)

- Use a pinned, optional [Play!](https://github.com/jpd002/Play-) core as the
  implementation base. Its built-in HLE BIOS means the normal WhittyArcade
  path does not need a Sony or Namco BIOS dump.
- Keep the board behind `WHITTY_ENABLE_SYSTEM246` until ROM audit, boot,
  video, audio, input and repeated-session tests pass on Windows and Linux.
- Use [PCSX2X6](https://github.com/PS2Homebrew-arcade/pcsx2x6) only as an
  external comparison implementation. It is GPL-3.0 and must not be copied or
  linked into a proprietary WhittyArcade build.
- Use MAME's
  [`namcops2.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/namco/namcops2.cpp)
  as the primary hardware, ROM-name and dump-audit reference. MAME currently
  marks the System 246/256 machines not working and without sound, so it is
  not a runtime backend.
- Never bundle a game, dongle, CHD, arcade BIOS or derived media in source,
  CI artifacts or a release. All game data remains user supplied.

This is deliberately an emulator-core integration, not a new PS2 emulator.
Reimplementing the Emotion Engine, IOP, VUs, GS, SPU2, optical/HDD devices,
Namco ACRAM and JVS stack would be a separate multi-year project.

## Validated first game

The supplied files were inspected read-only and then copied to an isolated
temporary test directory. The originals were not modified.

| Asset | Validated identity |
| --- | --- |
| `rrvac.zip` | SHA-256 `6920c37444919d8cf67c3d0ed69d050f0f1b5e018931fecb3d6dd7b643ebb47b` |
| `rrv1-a.chd` | SHA-256 `ab7e7575e7ead695f7d777239cf6f42efe04394a33ca4ac9a240ee5712b7fe5f`; CHD SHA-1 `77bb70407511cbb12ab999410e797dcaf0779229` |

The ZIP contains the RRV3 Ver.A dongle expected by both MAME and Play!:
`rrv3vera.ic002`, 8 MiB, CRC-32 `dd20c4a2`. It also contains the following
MAME-audited board data:

| Member | Size | CRC-32 | Note |
| --- | ---: | --- | --- |
| `rrv3vera_spr.ic002` | 262,144 | `712e0e9a` | Dongle spare area |
| `fcap11.ic2` | 16,400 | `1b2592ce` | JVS board data |
| `fcaf11.ic4` | 49,152 | `9794f16b` | MAME marks this known dump `BAD_DUMP` |
| `fcb1_io-0b.ic4` | 49,152 | `5e25b73f` | MAME marks this known dump `BAD_DUMP` |
| `rrv3_str-0a.ic16` | 524,288 | `df8b6cac` | Steering/feedback board data |

MAME accepted every supplied ROM and the CHD identity. Its audit lacked only
the parent System 246 BIOS `r27v1602f.7d`; that BIOS is not needed by the
chosen Play! path. The two `BAD_DUMP` flags above are properties of MAME's
current reference definitions, not newly discovered corruptions.

### CHD format caveat

The supplied, MAME-correct CHD has `GDDD` hard-disk metadata even though its
logical payload is a valid ISO-9660 image (`SYSTEM246`, volume `RRV1_A`). Play!
expects optical `CHT2` metadata for a `cdvd` asset and otherwise reports
`invalid ISO9660`.

A temporary conversion with MAME `chdman` 0.288 proved the data is usable:

```sh
chdman info -i rrv1-a.chd
chdman extracthd -i rrv1-a.chd -o rrv1-a.iso
chdman createcd -i rrv1-a.iso -o rrv1-a-play.chd
chdman verify -i rrv1-a-play.chd
```

The extracted logical ISO was 258,684,928 bytes with SHA-256
`f6a7579da1033ee9b57ebed92ce5c04ab4088f483983e85d845b08d33bffe38b`.
The resulting one-track MODE1 CHD had `CHT2` metadata and passed raw and
overall SHA-1 verification. Compression output hashes are not a portable
asset contract because they can change with `chdman` versions and options.

For the product, prefer a small upstreamable Play! change that recognizes a
`GDDD` CHD with 512-byte units and presents its logical byte stream to
`COpticalMedia::CreateAuto`. That lets users select the original MAME set
without generating a second copy. A cached private ISO conversion is an
acceptable fallback, but the UI must state the extra disk-space requirement
and never overwrite the source CHD.

### Empirical boot result

The unchanged dongle ZIP plus the temporary optical-format CHD booted with
Play! 1.0.0.33 through Wine on the Linux development host. Play! loaded the
`START` executable and rendered the Ridge Racer V: Arcade Battle title/coin
screen. This establishes asset and baseline-emulator feasibility; it is not
yet evidence that WhittyArcade's future adapter, every race mode, audio,
force feedback or long-session stability works.

## Upstream comparison

| Project | What the research established | Role here |
| --- | --- | --- |
| Play! | Permissive two-clause license, HLE BIOS, native System 246 driver, CHD/dongle mounting, ACRAM/ACATA/ACCDVD/JVS HLE, driving input and three RRV-specific patches. The supplied game reached its title screen. | Recommended pinned core after a complete transitive-license audit. |
| PCSX2X6 | GPL-3.0 PCSX2 fork; its compatibility tracker reports RRV playable. It requires a user-dumped System 246/256 COH-H BIOS because a retail PS2 BIOS has incompatible arcade modules. | External developer baseline only; do not ship, link or copy it into WhittyArcade. |
| MAME | Best current ROM definitions and board documentation. The RRV driver identifies the V257 steering/feedback board and the real cabinet's wheel/feedback startup checks. Runtime is marked `MACHINE_NOT_WORKING | MACHINE_NO_SOUND`. | Audit/reference only; adapt only individually licensed BSD-3-Clause material with notices and provenance. |

The PCSX2X6 compatibility source is its
[RRV tracking issue](https://github.com/PS2Homebrew-arcade/pcsx2x6/issues/9).
Its arcade BIOS requirements are documented in
[Getting a BIOS](https://ps2homebrew-arcade.github.io/pcsx2x6/getting_a_bios/).
Play!'s placement rules, driving controls and CHD troubleshooting are in its
[official README](https://github.com/jpd002/Play-/blob/master/README.md).

## Proposed integration

### Build and dependency boundary

Add an opt-in `whitty_playcore` target pinned to a reviewed Play! revision.
Do not link Play!'s Qt frontend. Its existing `ui_shared` target also pulls in
SQLite, HTTP and optional S3/library features that the emulator session does
not need, so the clean endpoint is a small `PlayArcadeCore` target containing:

- `PlayCore` and the System 246 IOP/HLE implementation;
- the Namco System 246 arcade driver and definition parser;
- CHD and ZIP readers;
- a narrow host API for boot, frame execution, input, PCM output, saves and
  GS-frame delivery.

Prefer contributing that target/API upstream. If it must initially live as a
WhittyArcade patch, keep the patch set small, documented and reproducible
against the pinned revision. Configure `ENABLE_AMAZON_S3=OFF`.

Before redistribution, record the license of Play! and every linked
dependency in `THIRD_PARTY_NOTICES`. The Play! root license is compatible in
principle, but that does not replace a file-by-file/transitive dependency
audit.

### Catalog and loader

1. Add `arcade_board_type::system246`, a `system246` descriptor and the
   `rrvac` manifest.
2. Add `system246_rom.h/.cpp`. Identify the set by the dongle member name,
   size and CRC, not by the archive filename.
3. Extend the ROM-library manifest contract to represent non-archive media
   explicitly. For `rrvac`, resolve:

   ```text
   rrvac.zip
   rrvac/rrv1-a.chd
   ```

   Both merged and a user-selected game directory can be supported, but the
   resolved files must be unambiguous.
4. Audit the CHD header before launch. Accept native `CHT2`; accept this known
   `GDDD` image through the stream adapter/cache path; reject HDD media passed
   to unrelated CD/DVD definitions with a precise explanation.
5. Keep paths in local configuration. Store only identity hashes, metadata
   and relative dependency names in catalog/source code.

Play!'s RRV definition boots `ac0:START`, selects driving JVS mode and applies
these required patches:

| Address | Value | Purpose |
| --- | --- | --- |
| `0x00236d84` | `0` | Disable i.Link initialization |
| `0x00229bd8` | `0` | Skip brake/gas startup check |
| `0x0026ce38` | `0` | Prevent the pre-attract-mode hang |

Keep those values in game metadata with provenance, not as anonymous writes
inside the session loop.

### Session and host services

Add `system246_session.cpp`, derived from `video_emulator_session`, plus one
`make_system246_session()` declaration and one central factory branch. The
session owns the Play! VM, handlers, save path and all emulator threads.

- **Video:** provide a custom/offscreen Play! GS handler. Start with a
  correctness-first RGBA/staging path that submits complete immutable frames
  to `arcade_video_worker`; then add Vulkan texture sharing if profiling shows
  the copy is material. Play! must not create a second production window or
  own WhittyArcade's process-wide SDL/Vulkan lifetime.
- **Audio:** provide a Play! sound handler that sends bounded stereo PCM to
  `arcade_audio_output`. Pause, reset and teardown must stop the producer
  before the shared output is flushed.
- **Input:** adapt the existing neutral `input_state`; no new host input model
  is needed. Map steering to left-stick X, gas to positive left-stick Y,
  brake to positive right-stick X, coin to Select, start to Start, and
  service/test to the corresponding JVS actions. Test endpoints and neutral
  values so the startup checks cannot see pedal or wheel drift.
- **Persistence:** redirect Play!'s backup RAM and arcade save files into the
  WhittyArcade per-game configuration root. Never use Play!'s global desktop
  data directory from an embedded session.
- **Lifecycle:** pause and join the VM, sound and GS producers before calling
  `arcade_video_worker::reset_session()`. Recreating `rrvac` among all other
  boards 25 times must leave no thread, RAM, audio queue or callback alive.

## Delivery phases and gates

### 0. Reference baseline — complete

- Audit the supplied set against MAME.
- Explain and correct the GDDD/CHT2 mismatch without touching the original.
- Reach a real RRV title frame in Play!.

### 1. Dependency and headless-core spike

- Pin a clean upstream Play! revision.
- Finish the transitive-license and notice audit.
- Build `PlayArcadeCore` without Qt, library databases, HTTP or S3 on Windows
  and Ubuntu.
- Prove create/boot/pause/reset/destroy through a host-owned API.

Gate: a no-ROM smoke executable links on both platforms and a local ROM-backed
test reports that `START` executed.

### 2. ROM-library integration

- Add the board/catalog/loader entries and structured CHD dependency.
- Implement content-based ZIP audit and CHD metadata/identity checks.
- Add direct GDDD logical-stream support or a non-destructive cache fallback.

Gate: missing dongle, wrong revision, missing CHD, wrong media type and valid
RRV each produce deterministic test results and useful UI messages.

### 3. Whitty session output

- Wire the offscreen GS frame sink, PCM sink, frame pacing and pause/reset.
- Prove non-black changing frames without committing copyrighted screenshots
  or game data.
- Verify audio bounds, underrun recovery and teardown under sanitizers where
  practical.

Gate: boot to the title/attract sequence through WhittyArcade with one window,
one shared video worker and clean audio shutdown.

### 4. Cabinet controls and persistence

- Map wheel, gas, brake, coin, start, service and test.
- Verify two-coin credit behavior, service-menu navigation, analog endpoints,
  backup-RAM save/reload and a complete race.
- Treat force feedback as a separate optional capability; ignore commands
  safely until a reviewed SDL haptics design exists.

Gate: cold boot, attract, credit, game start, race control and restart all
work with keyboard and an SDL controller/wheel profile.

### 5. Hardening and release eligibility

- Run the normal no-ROM suite plus local `SYSTEM246_TEST_ROOT` ROM-backed
  tests.
- Add Windows and Ubuntu compile/link/smoke jobs to GitHub Actions. ROM-backed
  tests remain local and never upload assets or derived caches.
- Pass 25 cross-board session boundaries, a 30-minute attract run and a
  complete-race soak with bounded memory/audio queues.
- Add third-party notices, user dump-placement/conversion help and an explicit
  experimental-support label until the gates above pass.

Only then mark the catalog manifest working and include the optional core in
release packaging.

### 6. System 256 expansion

After RRV is stable, select a System 256 title based on legally supplied
media. Reuse the same Play! VM boundary and add only its actual HDD/DVD,
dongle, JVS and patch differences. Do not claim generic System 256 support
from one System 246 game.

## Test split

Tests safe for public CI:

- catalog uniqueness, board routing and manifest dependency rules;
- ZIP/CHD parser tests built from synthetic data;
- handler API, input scaling, audio bounds and lifecycle tests;
- Windows/Linux builds with the optional backend enabled but no game data.

Local ROM-backed tests, enabled only by `SYSTEM246_TEST_ROOT`, should prove:

- exact `rrvac` identity and media resolution;
- `START` execution and changing GS frames;
- attract-mode survival, coin/start and analog JVS input;
- backup-RAM persistence and repeated cross-board teardown.

The test root stays in the CMake cache/environment and must never be printed
in release metadata, embedded in binaries or uploaded by CI.

## Remaining user-supplied material

Nothing else is required for the recommended Play! implementation: the
validated RRV ZIP and CHD contain the necessary game data, and Play! uses an
HLE BIOS. A real wheel/controller is useful later for cabinet testing but is
not a dump requirement.

A System 246/256 COH-H BIOS would be needed only for an optional developer
comparison with PCSX2X6. A retail PS2 BIOS is not a substitute. The missing
MAME parent BIOS is likewise not a blocker for the Play!-based port.

## Known issue: RRV streamed engine audio stalls after one chunk

Status: root cause narrowed but not fixed as of 2026-07-23. Symptom: engine
revs and other SPU2-core-1 streamed ADPCM audio (voices 21-23) play correctly
for the first loaded chunk, then the same stale ~0x8000-byte buffer repeats
indefinitely, audible as an escalating buzz/noise. The disc-based BGM
(`system246/rrv_music.cpp`, decoded independently on the host, not through
the emulated SPU2 at all) is unaffected and continues to play correctly.

### What is confirmed

- `Iop::Namco::CAcCdvd` (`src/../third_party/play/Source/iop/namco_sys246/Iop_NamcoAcCdvd.cpp`)
  answers every ACCDVD call synchronously inside the SIF RPC handler itself.
  Across a full multi-minute session, RRV calls it exactly six times (init,
  status, `CdSync`, `SearchFile`, one bulk `Read9`, one `0x0A` direct read)
  and **never again** — including never calling the class's own dedicated
  streaming methods `0x13`/`StRead` and `0x15`/`StSeek`, which exist and
  work but sit completely unused.
- The hijack that routes all this traffic to our C++ class is
  `sif.RegisterModule(MODULE_ID /* 0x76500002 */, this)` in `CAcCdvd`'s
  constructor — a direct SIF-server-ID claim, not the
  `RegisterHleModuleReplacement("ATA/ATAPI_driver", ...)` /
  `("CD/DVD_Compatible", ...)` calls in `play_core.cpp`, which target module
  names that don't match anything on this disc and appear to be dead code
  for RRV specifically (see next point).
- The genuine on-disc IOP driver chain was identified from real `.iopmod`
  names embedded in five extracted IRX modules (provenance: a live IOP-RAM
  dump from an earlier session, not a plain disc scan — see below):
  `ATAPI_C/DVD_driver` (acacd) → `CDVD_compatible_library` (accdc) →
  `CDROM_fileio_driver` (accddrv) / `CDVD_compat_lib/IOP` (accdvd) →
  `CDVD_compat_serv/EE` (accde, the genuine EE-facing SIF RPC service Play!'s
  `CAcCdvd` is standing in for). `acacd`'s own dependency list is `acacd,
  aclocore, acata, sysmem, intrman, sysclib, thbase`.
- `sysmem`, `intrman`, `sysclib`, `thbase` in that list are ordinary IOP
  kernel modules, HLE'd in Play! via the plain `CIopBios::RegisterModule()`
  path used for every built-in module (`IopBios.cpp` around the
  `RegisterModule(m_sysmem)` / `RegisterModule(std::make_shared<Iop::CThbase>...)`
  block). `acata` sits in the same import list alongside them, which is
  strong evidence it is *also* an ordinary HLE'd kernel-shaped module
  (resolved through the same import-linking/HLE-trampoline mechanism CModule
  already provides), not a raw memory-mapped hardware device and not a
  second SIF RPC server. Play! already has a same-shaped class,
  `Iop::Namco::CAcAta` (`Iop_NamcoAcAta.cpp/h`, upstream-committed, currently
  unused by WhittyArcade) — but it is wired for a flat HDD image
  (`SetHddStream`, plain LBA `READ_DMA`/`IDENTIFY_DEVICE`/`SET_FEATURE`), not
  optical media, and nothing in `play_core.cpp` constructs or registers it.
- Real ATA/ATAPI hardware-register emulation (the PCSX2X6 approach —
  `ACATA.cpp`/`ACATAPI.cpp`/`ACCORE.cpp`, real memory-mapped register I/O,
  no SIF layer at all) does not exist in Play!; `Iop_Dev9.cpp` is a ~50-line
  stub that only answers one identification register. This is not needed if
  `acata` turns out to be import-linked rather than MMIO-based (see above).
- The disc's plain ISO9660 filesystem (extracted from `rrv1-a.chd` and
  scanned directly) contains **zero raw ELF headers anywhere** — RRV's game
  data is stored in Namco's own packed/compressed container, decoded only by
  the game's own boot-time loader. This means the earlier IRX extraction did
  not come from a simple disc read, and it is *not yet verified* whether
  `CIopBios::LoadModuleFromPath`'s normal `m_ioman->Open()` path can already
  see these files once the game's own loader has staged them, or whether
  something else Play! doesn't replicate is needed first. This is the
  biggest open unknown and the reason a spike phase comes first below.
- PCSX2X6's own compatibility tracker lists RRV as fully "PLAYABLE" with no
  audio caveats, and its arcade CD/ATA emulation is genuinely low-level
  (register-level, not RPC-level), so it cannot serve as a source of the
  exact SIF-level protocol semantics Play!'s shortcut needs to match — it
  sidesteps that layer entirely by not needing it. It does, however, confirm
  this is a solved problem on real/faithful hardware, not an inherently
  broken case.

### Proposed fix — updated 2026-07-23 with a verified module map and a known blocker

Replace `CAcCdvd`'s complete SIF-RPC-level shortcut for RRV with a much
smaller, lower-level HLE that lets the genuine on-disc driver chain load and
run unmodified, talking to emulated low-level libraries exactly the way
`sysmem`/`thbase` are already emulated today. The real drivers already
contain whatever correct streaming/completion protocol RRV expects; this
avoids having to reverse engineer and re-implement that protocol at the SIF
level by guesswork (the approach that consumed most of a session without a
confirmed fix).

**The real modules were located and are not on the disc — they are plain,
uncompressed ELFs embedded directly in the 8 MiB dongle
(`rrv3vera.ic002`, byte-identical to `system246_play_core::implementation
::dongle` — confirmed via `src/system246_rom.cpp:21` `dongle_name` and the
`read_dongle` path).** Verified exact byte offsets (scan any dongle copy for
`.iopmod` section names; these will not move for this specific dongle
build):

| Offset | `.iopmod` name | Import-name | Role |
| --- | --- | --- | --- |
| `0x267CA0` | `ATA/ATAPI_driver` | — | Not imported by short name anywhere found; likely dead for this fix (see below) |
| `0x26A030` | `ATAPI_C/DVD_driver` | `acacd` | ATAPI command layer; imports `aclocore, acata, sysmem, intrman, sysclib, thbase` |
| `0x26CCB0` | `ISO9660_filesystem` | `accdfs` | Imports `aclocore, acacd, sysmem, loadcore, stdio, sysclib, thbase, thsemap` |
| `0x26F300` | `CDVD_compatible_library` | `accdc` | Imports include `aclocore, acata, acacd, accdfs, sysmem` (list not fully re-verified this session) |
| `0x274180` | `CDVD_compat_serv/EE` | `accde` | The genuine EE-facing SIF RPC service `CAcCdvd` currently impersonates under server ID `0x76500002`. Imports `aclocore, acram, accdc, cdvdman, intrman, sifcmd, sifman, stdio, sysclib, thbase` |
| `0x2609B0` | `Arcade_CORE` | `aclocore` | Imports `acdev, ioman, loadcore, stdio, thbase`; loads an FPGA bitstream from `mc0:FPGA` at real-hardware init (`acfpga:load:wait`, `aclocore:fpga:` strings) — real hardware-touching code, do **not** load this one for real |

`accddrv`/`CDROM_fileio_driver` (`0x26E980`) and `accdvd`/`CDVD_compat_lib/IOP`
(`0x273360`) exist on the dongle too but are **not** in `accde`'s import list
and were not needed for this dependency chain.

**Confirmed empirically this session (env `WHITTYARCADE_TRACE_AUDIO=1`):**
- `accde` (the real SIF-0x76500002 service) is never loaded and never
  attempts to self-register — `Source/ee/SIF.cpp` `CSIF::RegisterModule` was
  instrumented for IDs `0x76500000-0x7650000F` and only ever saw the single
  startup registration from our own `CAcCdvd`/`CSys246` constructors, never a
  second/competing one. The game architecturally expects `CD/DVD_Compatible`
  to already exist (BIOS-resident on real hardware), matching how retail
  PS2's `cdvdman` is BIOS-resident — it is *not* something the game loads
  itself, so nothing will make it appear without us explicitly loading it.
- `CAcCdvd::Invoke(CMIPS&, unsigned int)` (the `CModule`/import-style
  interface reachable via the `"ATA/ATAPI_driver"` name hijack in
  `play_core.cpp`) is **never called** either, even after 7+ in-game track
  changes — so that hijack is currently dead code for RRV, and the module
  named literally `ATA/ATAPI_driver` is not what `acacd` imports (`acacd`
  imports the *short* name `acata`, a completely separate identifier space
  from the `.iopmod` name — see next point).
- Cross-module import calls resolve through a **plain `std::string`-keyed
  map**, `CIopBios::m_modules` (`IopBios.cpp:3529` `RegisterModule`, keyed by
  `GetId()`), looked up by walking backwards from the call site to a
  `0x41E00000` header marker and reading a short name
  (`IopBios.cpp` ~line 3300-3325, `ReadModuleName`). This is the exact same
  mechanism that already resolves `sysmem`/`intrman`/`thbase`/etc. for every
  other HLE'd kernel module. `Iop::Namco::CAcAta::GetId()` already returns
  exactly `"acata"` (`Iop_NamcoAcAta.cpp:54`) — it is upstream-committed and
  completely unused by WhittyArcade today. A new `Iop::Namco::CAcLoCore`
  stub for `"aclocore"` was written this session
  (`Iop_NamcoAcLoCore.h/.cpp`, added to `Source/CMakeLists.txt`) but is
  **not wired into `play_core.cpp`** — it exists only as an inert starting
  point.
- `CIopBios::LoadModuleFromHost(uint8*)` (`IopBios.cpp:679`) successfully
  loaded `acacd` directly from `dongle.data() + 0x26A030` with no crash and
  no observable side effects (returned a valid module id, game continued
  booting normally) — but `LoadModule()` only relocates and marks the module
  `MODULE_STATE::STOPPED`; it does **not** run anything.

**Bootstrap-thread blocker — SOLVED this session.** The earlier segfault
from calling `StartModule(LOCAL, ...)` directly from `prepare_environment`
was root-caused precisely: `RequestModuleStart` (`IopBios.cpp`) calls
`SleepThread()` when `requestSource == LOCAL`, which does
`GetThread(m_currentThreadId)` — an out-of-bounds access when
`m_currentThreadId == -1` (no thread running yet, which is always true at
`prepare_environment` time, before `vm->Resume()` first ticks). `REMOTE`
avoids `SleepThread()` but isn't safe either: at completion,
`FinishModuleStart` sees `requesterThreadId == -1` and calls
`m_sifMan->SendCallReply(Iop::CLoadcore::MODULE_ID, nullptr)`, which asserts
if no genuine SIF RPC placed a matching entry in `m_callReplies` (harmless
only by accident, if built with `NDEBUG`).

The fix: a third `MODULESTARTREQUEST_SOURCE::HOST` value
(`IopBios.h`/`IopBios.cpp`), used only for module starts triggered directly
from host C++ with no live IOP thread and no originating SIF RPC. It skips
`SleepThread()` like `REMOTE`, but stores a distinct sentinel
(`MODULESTARTREQUEST_NO_REQUESTER = -2`) instead of `-1`, so
`FinishModuleStart` recognizes it and skips the `SendCallReply` notification
entirely — there's nothing to notify. `TriggerCallback`'s `CreateThread`/
`StartThread` calls (used by `RequestModuleStart` to spawn the actual
"module starter" thread that runs the module's entry point) only touch
`CIopBios`-internal bookkeeping (`m_threads`, `m_sysmem`) and don't depend on
`m_currentThreadId` at all, so this is safe to call from
`prepare_environment`, right after `vm->Reset()` (which is what initializes
`m_moduleStarterProcAddress` and friends) and before `vm->Resume()`.
Confirmed via the new `System 246 ProcessModuleStart`/`FinishModuleStart`
traces (env `WHITTYARCADE_TRACE_AUDIO=1`, added to `IopBios.cpp`): calling
`iop_bios->LoadModuleFromHost(dongle.data() + offset)` then
`iop_bios->StartModule(CIopBios::MODULESTARTREQUEST_SOURCE::HOST, id, name, "", 0)`
for all four modules from `prepare_environment` no longer crashes, and each
module's real entry point genuinely executes on the MIPS interpreter once
`vm->Resume()` starts ticking.

**A note on `MODULE_RESIDENT_STATE` — do not repeat this misreading.** The
enum (`IopBios.h`) is `RESIDENT_END = 0, NO_RESIDENT_END = 1,
REMOVABLE_RESIDENT_END = 2`. A `residentState=0` trace value means the
module *succeeded* and stayed loaded — it is **not** a failure/bail-out
code, despite the tempting-but-wrong reading of "0 = nothing happened."

### ATAPI protocol reference — learned from PCSX2X6 as a read-only reference, not copied

A licensed real System 246 BIOS (`r27v1602f.7d`, user-supplied, matches the
identity the project's MAME audit already flagged as the missing parent set)
is now available, which unblocked building and reading PCSX2X6 as a genuine
working oracle (`docs/system246_256_board_plan.md`'s existing policy: PCSX2X6
is GPL-3.0, comparison/reference only, **never linked into or copied into
WhittyArcade** — confirmed again this session; its core cannot be embedded
in-process without making the combined binary GPL-3.0, which is out of
scope). PCSX2X6 built cleanly on this machine using system Qt6/SDL3/ffmpeg
via plain `find_package()` (`cmake -B build -G Ninja
-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`, no bundled
dependency build needed) — useful to know if a future session wants to
re-run it as an oracle. Getting it to actually *execute* headlessly for
log capture was not completed and is not needed for what follows: reading
its `DEV9/ACATA*.cpp`/`.h` sources directly (a from-scratch, register-level
ATA/ATAPI hardware model — completely different in shape from anything
Play! has) already answered the open protocol questions.

**The command surface is small and is the public SCSI MMC command set, not
proprietary Namco protocol** — safe to reimplement independently:
`TEST_UNIT_READY` (0x00), `INQUIRY` (0x12), `READ_CAPACITY` (0x25),
`READ_10` (0x28), `MODE_SELECT` (0x55), `MODE_SENSE` (0x5A),
`SET_STREAMING` (0xB6), `SET_CD_SPEED` (0xBB). `ACATAPI.cpp:346`'s
`ACATAPI::Setup()`/`handle_cmd()` is the full command dispatch — nothing
else is handled for CD/DVD media.

**The critical fact this reference revealed that the SIF-level black box
could never show:** `ACATAPI.cpp:247-260`'s handler for `READ_10` explicitly
documents `nsec == 0` (zero sectors requested) as **not an error** —
*"CD games spam it while streaming"* — i.e. a real title's disc driver polls
readiness with zero-length reads as an ordinary part of steady-state
streaming, separate from actual data-bearing reads. `SET_STREAMING` (0xB6)
is the real command that arms continuous read-ahead in the first place.
**Neither concept exists anywhere in Play!'s current SIF-level `CAcCdvd`
interface** — there is no equivalent of a zero-length "ready?" poll, and
none of its stub methods (`0x0D` "Seek?", `0x0F` "Cmd15()") were ever
verified against this specific semantic. This is the most likely reason the
EE-side refill logic (independently, exhaustively traced this session at
the SIF level) never re-triggers: whatever low-level poll/arm sequence a
genuine `accacd`/`accdc` driver would perform is invisible at the SIF layer
because those modules never run under Play!'s HLE-everything shortcut.
Two other useful, load-bearing details from the same read: `READ_10`'s DMA
path completes **synchronously** in one call (`atapi_complete_nodata()`
right after buffering, `ACATAPI.cpp:276-278`) with a comment about avoiding
"a race where SPU2 DMA completes before disc data is ready" — this
independently validates that `CAcCdvd`'s existing synchronous-completion
design for method `0x0A` is *not* the bug; and PIO chunked transfers are
paced by a per-tick `chunk_poll()` (`ACATAPI.cpp:119-132`) that waits a few
ticks before offering the next DRQ chunk, which is one plausible model for
how real timing-sensitive completions should be paced if that turns out to
matter later.

### Modular design for a clean-room reimplementation

Two independent pieces, each testable without booting the VM:

**1. `system246::atapi_registers` — DONE this session, unit-tested,
not yet wired to anything live.** `src/system246/atapi_registers.h/.cpp` +
`tests/system246_atapi_test.cpp` (15 assertions, all passing; wired into
`CMakeLists.txt` as the `system246_atapi`/`system246_atapi_test` targets
and into the `check_system246` aggregate). A pure ATA/ATAPI **command
state machine** with zero filesystem/host-I/O/Play! dependency — takes an
`atapi_packet` plus an injected `atapi_sector_reader` callback, returns an
`atapi_command_result {error, data_length}` and exposes the resulting
`pio_buffer()`. Implements the 8-command SCSI MMC subset from the section
above (`TEST_UNIT_READY`, `INQUIRY`, `READ_CAPACITY`, `READ_10`,
`MODE_SELECT`, `MODE_SENSE` pages `0x01`/`0x2A`, `SET_STREAMING`,
`SET_CD_SPEED`), including the zero-sector `READ_10` case as an explicit,
tested, first-class success path (`test_read_10_with_zero_sectors_is_not_an_error`)
— the specific behavior this whole investigation turned on. Deliberately
does **not** model real ATA task-file registers
(`status`/`error`/`LCYL`/`HCYL`/`NSECTOR`) or interrupt timing yet; that is
the next layer, described below, once this is wired to something live.

**Built this session:** `CAcAtaAtapi` (`Iop_NamcoAcAtaAtapi.h/.cpp`,
`Iop::Namco`, `GetId() == "acata"`) owns an `atapi_registers` instance,
mirrors `CAcAta`'s `AtaSetup`/`AtaRequest` register-array parsing, handles
`ATA_COMMAND_PACKET` (0xA0) by treating the AtaRequest data buffer as
carrying the 12-byte CDB directly (this project's own convention for this
wrapper API, not raw ATA/ATAPI hardware — there's no separate PIO data-out
phase to place it in), and resolves reads against `COpticalMedia` via
`GetBlockProvider()`. Registered in `prepare_environment` alongside
`CAcLoCore` (now wired, still a pure stub). **Not yet exercised by real
traffic** — see the aclocore-framework finding below for why.

**Bootstrap thread — solved,** see above. `prepare_environment` now loads
and starts `acacd`/`accdfs`/`accdc`/`accde` from the dongle in dependency
order via `load_and_start_dongle_module()`/`load_real_driver_chain()`
(`play_core.cpp`), each guarded so a load/start failure logs and falls back
to the existing `CAcCdvd` stub rather than crashing boot. Gated behind
`WHITTYARCADE_DISABLE_REAL_DRIVER_CHAIN` (unset by default) for A/B testing.

**New finding this session — the "aclocore framework" problem (the real
remaining blocker for this whole approach).** All four real modules'
entry points (confirmed via read-only MIPS disassembly of the dongle ELF —
capstone, no PCSX2X6 or any GPL code involved, purely this project's own
locally-licensed dongle) turned out to be trivial: e.g. `acacd`'s entire
`_start` is `aclocore_func6(moduleBase, moduleBase+rangeSize, 1); return`
— it delegates its *entire* init to one call into "aclocore" and returns
*that call's own result* as its own resident-state. `ProcessModuleStart`/
`FinishModuleStart` traces confirm this empirically: all four modules reach
`MODULE_STATE::STARTED` (residentState=0=`RESIDENT_END`, i.e. success) but
none of them ever call `RegisterLibraryEntries` (traced in
`Iop_Loadcore.cpp`, zero hits) or reach `CAcAtaAtapi::Invoke` (zero
`ACATA AtaRequest` traces across multiple multi-minute live runs). The
likely real architecture: `aclocore.irx` isn't just a passive stub these
modules poll — it's a **framework** these board-driver modules register
with once (passing their own code/data range), and the real `aclocore`
would inspect that range, then **call back into the driver** at some later
point (spawning its worker thread, calling its own `RegisterLibraryEntries`,
etc.) as part of accepting the registration. A blanket "return 0" stub
can't do that follow-up, so the modules stay resident but permanently
inert. Making the real driver chain functionally active would require
reverse-engineering that undocumented callback/descriptor protocol — a
substantially bigger undertaking than scoped for this fix, and not
attempted further this session.

**Live A/B confirmation this session: the new driver-chain code has zero
effect on actual audio behavior, in either direction.** Running the full
`system246_rrv_stream_integration_test` (see below) with
`WHITTYARCADE_DISABLE_REAL_DRIVER_CHAIN=1` produces byte-identical results
to running with it enabled — same 44s sustained healthy audio window, same
windows, same zero clipping throughout. This is expected given the finding
above (the loaded modules never become functionally active either way) and
confirms this session's module-loading work introduces no regression, but
also means **it is not what's keeping RRV's streamed audio healthy** — see
"Live verification" below for what that actually shows.

### Live verification — the audio bug does not reproduce in a real 4-minute race (2026-07-23)

Built `tests/system246_rrv_stream_integration_test.cpp`: boots the real
`system246_play_core` directly (no `arcade_frontend`/window — respects the
project's "don't launch the graphical app" testing-split convention) against
the real `rrvac.zip` dongle + `rrv1-a.chd`, mutes the real output device
(`audio_control->set_paused(true)`, so nothing reaches speakers) while
`WHITTYARCADE_SPU_DUMP` still captures the genuine pre-mix SPU signal, and
scripts cabinet input to actually reach and sustain a race:

- The real boot sequence (self-test "keep hands off steering wheel"
  hardware check → parental advisory → attract cinematic) runs for ~45
  real seconds **on its own, with zero effect from any input** — confirmed
  by capturing BMP screenshots via `capture_latest`/`RRV_TEST_SCREENSHOT_DIR`
  and by an A/B test showing identical audio traces with/without a
  coin/start input script during this phase. Coin/start presses before
  ~2850 frames are simply too early for the game to ever see them.
- This cabinet costs **2 coins**, not 1 (confirmed via screenshot:
  "CREDITS 1/2" after a single held pulse) — needs two separate rising
  edges, not one held press.
- With correctly-timed input (2 coin pulses ~t47s, repeated start pulses
  ~t51-71s to click through course/car select with default choices, full
  throttle held from ~t75s), the game reaches a real race
  (screenshot-confirmed: "Round 1, PARK TOWN" HUD with tachometer,
  speedometer, lap timer, position).
- The resulting SPU dump shows a **44-second continuous sustained
  streaming segment (t=106s-148s) with 0.000% sample clipping in every
  single 2-second window across the entire ~240-second recording** — no
  silence-within-the-run, no clipping spike, no garbage/noise signature.
  This is the exact scenario the original bug describes ("plays correctly
  briefly then degrades into noise") and it does not degrade.
- The test's verdict logic (see the file) specifically distinguishes this
  from a stall: it finds the longest contiguous run of "something is
  audible" windows (menus/select-screens are legitimately silent between
  one-shot blips — that's not a bug) and only fails if clipping or
  near-total-silence appears *partway through* that sustained run.

**Interpretation:** the streamed-audio bug is very likely already fixed by
earlier changes in this session (the `PS2OS.cpp` thread-context-corruption
fix and/or the `prepare_environment` SPU/DMA tuning — `SetVoiceDmaWriteThrottlingEnabled(false)`,
the reduced `PREF_AUDIO_SPUBLOCKCOUNT`), which predate the real-driver-chain
work and were confirmed unaffected by the A/B test above. This has **not**
been re-tested against a *pre-those-fixes* build in this session, so that
attribution is inferred, not directly re-verified. What this session does
newly and directly establish: a real, repeatable, automated regression
test that boots the actual game to a live race and would catch this
specific bug if it recurred.

### BIOS verdict — "do what PCSX2 does" is not possible inside Play! (2026-07-24)

The user supplied the genuine System 246 BIOS (`r27v1602f.7d`, 2 MB, in
`/home/jon/Downloads/sys246.7z` — matches the MAME parent-set filename) with
the intent of "getting the bios and doing what pcsx2 does," i.e. booting the
real firmware so the real driver chain loads naturally. **This is
structurally impossible in Play! and should not be attempted.** Play! is a
high-level-emulation core: `CPS2OS` (`Source/ee/PS2OS.cpp`) *hand-assembles*
its own minimal BIOS into the BIOS region (`CMIPSAssembler` writing interrupt
/ DMAC / INTC handlers, thread epilog, idle proc at `0x1FC00xxx`) and boots
the game ELF directly via `BootFromVirtualPath`/`LoadELF`. There is **no
`LoadBIOS(file)` path anywhere** — the EE never executes a real BIOS image.
Handing Play! `r27v1602f.7d` is a no-op. "Do what PCSX2 does" means running
the real BIOS, which requires a full low-level-emulation EE/IOP kernel —
i.e. PCSX2 itself. PCSX2X6 (`/tmp/pcsx2x6`) is a complete standalone Qt
application built around an x86-64 recompiler and the real COH-H BIOS, not an
embeddable core; adopting it for the System 246 board would be a multi-month
re-architecture with GPLv3 implications, not a patch. The realistic path for
this board stays inside Play!'s HLE.

### Finer-grained live analysis — the stream double-buffer never advances (2026-07-24)

Re-ran `system246_rrv_stream_integration_test` (9000 frames) on the current
tree with the full MA_MIPSIV instrumentation enabled. Results refine, and
partly qualify, the 2026-07-23 "healthy" conclusion:

- **No garbage/noise, confirmed again.** 0.000% sample clipping in every
  2-second window across the whole ~150 s recording; longest sustained
  audible run 34 s. The "goes bang / random noise" signature does not appear.
  This is consistent with the corruption being resolved by the earlier
  `PS2OS.cpp` thread-context fix + SPU/DMA tuning.
- **Real driver chain loads and stays resident.** `acacd`→`ATAPI_C/DVD_driver`,
  `accdfs`→`ISO9660_filesystem`, `accdc`→`CDVD_compatible_library`,
  `accde`→`CDVD_compat_serv/EE` all reach `FinishModuleStart` with
  `residentState=0` (`RESIDENT_END`, success). The `HOST`-source start path
  is stable (no crash) — the historical segfault was the `LOCAL`-source
  `SleepThread` path, now avoided.
- **Disc-read RPCs *are* submitted and complete.** 140 `cvd rpc-call`s
  (methods 2 and 0x0A, `count=16`, varying `start` sectors
  0x15e71/0x15361/0x4d03 into ring `0x62500`/`0x6a500`) and 66 completion
  callbacks. So the CD-stream subsystem is active for *something*.
- **But the race-engine stream's double-buffer is stuck.** The advance
  counter at `0x01EB6E48` never increments (0 counter-increment hits, 0
  refill hits across the whole run); the two buffer states (`gp-0x5840` /
  `gp-0x583c`) sit at `-2` (`0xfffffffe`), and the completion callback
  (`0x2C56B0`) only promotes states that are exactly `-1`, so a `-2` buffer
  is never advanced. The slot-lookup at `0x2C5990` searches for `contextTag`
  (=1) but the two candidate slots carry frozen tags 47/46, so it never
  matches and the advance routine `0x2C6A50` is barely entered (only ~5 gate
  checks over 32k stream-func ticks).
- **The earlier gate hypothesis is disproven.** The flag at `gp-0x4db8`
  (`flagAddr` resolved to `0x003e68b8`) reads `0x00000000` on every gate
  check — it is *not* set, so it is not what blocks the advance. The block is
  upstream: the tag-slot lookup never matches, so the advance/gate code is
  rarely reached at all.

**Net:** audio is clean but the streamed engine channel appears to run as a
*static loop* (steady ~450 peak / ~70 rms during the sustained race window)
rather than advancing with new chunks — matching the user's most recent
report ("can't hear car revving") more than the original "bang/noise." Open
question requiring the user's ear on-device: whether this static-stream
residual is audibly wrong, or whether that frozen `tag0/tag1` stream is a
separate idle channel and the audible engine (normal SPU keyon voices) is
already correct. The remaining fix, if needed, is either the aclocore
framework RE (above) or reverse-engineering the EE buffer-state transition
that should move `-2`→`-1` for this stream — both deep, neither a quick patch.

**MEASUREMENT CAVEAT — the integration test measures the wrong signal
(2026-07-24).** `WHITTYARCADE_SPU_DUMP` captures the **pre-mix** SPU render
output, *before* the System 246 line gain (`system246_spu_line_gain_percent
= 1600`, i.e. **16×**, in `src/system246/play_audio.h`) and final output
mixing. So "0.000% clipping in the dump" does **not** mean "clean on the
speakers." On-device the user reports the opposite of what the dump showed:
music gone and random amplified "coin bing / whoosh" garbage. Any future
audio verdict must be judged on the post-gain, post-mix path (or on-device),
not the raw SPU dump.

**REGRESSION BACKED OUT (2026-07-24).** An uncommitted experiment in the Play!
submodule (`Iop_SpuBase.cpp/.h`: added `CSpuIrqWatcher::CheckIrqRange` calls
on every DMA/FIFO transfer crossing the IRQ address, plus a rewrite of the
`CSpuBase::Render` IRQ acknowledge/clear logic so pending IRQs accumulate
instead of clearing each render; and `MA_MIPSIV.cpp` trace hooks) was an
attempt to make RRV's double-buffer streaming driver see the correct number
of SPU IRQs. It changes **global** SPU IRQ timing for both cores and, on
device, broke music and normal SFX into spurious-IRQ garbage. It was never
committed. Backed out via `git stash` in the submodule (message: "WIP RRV
SPU-IRQ stream-refill experiment ... broke music+SFX on device 2026-07-24")
to restore the last-good commit `2660e794`. The *concept* (System 246 sound
drivers pace off "buffer write reached IRQ address") may still be right, but
a correct version must be scoped to the streaming core/IRQ address only and
must not perturb the cores/addresses that music and one-shot SFX rely on —
and must be validated on the post-mix path, not the SPU dump.

### Test plan

- **Unit tests (no VM, no ROMs, run in normal `ctest`):**
  `atapi_registers` command-by-command — **done** (`system246_atapi_test`,
  15 assertions, all 8 opcodes including the zero-sector `READ_10` case and
  an unknown-opcode case). Still needed: `CAcLoCore`'s stub contract once it
  has real behavior to assert on, and the `COpticalMedia`-backed sector-read
  adapter against a fake filesystem double (the state machine itself is
  already proven against a fake reader; this is only the adapter's own thin
  glue). Follow the existing `tests/system246_*_test.cpp` fixture pattern,
  matching `system246_atapi_test.cpp`.
- **Integration test — done this session:**
  `tests/system246_rrv_stream_integration_test.cpp` /
  `system246_rrv_stream_integration_test` (CMake target), gated behind the
  `RRV_TEST_ROM_ZIP` cache variable (matching the existing
  `SHINOBI_TEST_ROM`-style pattern — not registered as a `ctest` case unless
  a real `rrvac.zip` path is configured; currently configured to
  `/home/jon/.local/share/WhittyArcade/roms/system246/rrvac.zip` on this
  machine). Boots the real core headlessly (no window), scripts real cabinet
  input through the actual boot/self-test/attract/coin/select sequence
  documented above, and asserts on the captured SPU dump using a
  sustained-run health check (see "Live verification" above). Confirmed
  passing (`ctest -R system246_rrv_stream_integration_test`, ~185s
  wall-clock at 10800 frames). Optional `RRV_TEST_NO_INPUT=1` and
  `RRV_TEST_SCREENSHOT_DIR=<dir>` env vars support the same kind of ad-hoc
  diagnosis used to develop this test (BMP frame capture via
  `capture_latest`, no PNG/image codec dependency needed).
- Still needed, not started: `CAcLoCore`'s stub contract once it has real
  behavior to assert on (blocked on the aclocore-framework reverse
  engineering above), and a `COpticalMedia`-backed sector-read adapter unit
  test against a fake filesystem double for `CAcAtaAtapi` specifically (the
  `atapi_registers` state machine itself is already proven against a fake
  reader).
- Re-run the full `ctest` suite and the existing 25x cross-board teardown
  soak before considering any of this shippable, per the project's
  established Delivery-phase gates above. Full suite confirmed 67/67 passing
  this session (`ctest --test-dir build`, ~234s wall-clock total, dominated
  by the new RRV integration test's ~185s).

### Non-goals for this fix

- Do not touch System 246 video/deinterlacing while this is in progress;
  it is a separate, already-resolved area (see repository history around
  `src/system246/video_deinterlace.cpp`).
- Do not attempt to build a general-purpose ATA/ATAPI DEV9 hardware model
  for arbitrary retail PS2 HDD titles; scope this strictly to what RRV's
  `acacd`/`accdc` chain needs.
- Do not try to acquire a System 246/256 arcade BIOS to run PCSX2X6
  directly; it was useful here only as a source-level and compatibility
  reference, not something to execute.

## Known issue: brief wrong-sounding audio right at race start (open, 2026-07-24)

Live device testing after the fixes above (thread-context corruption, SPU/DMA
tuning) found the original "bang and random noise mid-stream" symptom no
longer reproduces (see the 44s zero-clipping sustained-run result above), but
a **different, narrower** issue remains: "random sounds" specifically **right
as the race HUD/track first appears** (confirmed via direct user testing,
not just the automated test's amplitude-based checks, which cannot tell
"wrong content" from "correct content" — only clipping/near-silence).

**What's confirmed, via live EE-side disassembly of the licensed dongle code
(read-only, capstone, no GPL/PCSX2X6 code involved) plus new JIT-level trace
hooks in `MA_MIPSIV.cpp` at hardcoded EE program-counter addresses (the same
technique this project's `TraceRrvAcCdvdClientState` family already used):**

- At the exact "GO!"/BGM-command-8 transition, RRV's own engine-audio
  bookkeeping struct (two slots at EE addresses `0x01EB6E00`/`0x01EB6E50`,
  `+0x00`=active, `+0x01`=category tag, `+0x28`=active2, `+0x48`/`+0x4A`=
  counter/threshold) needs a slot whose tag matches the new race's category
  (confirmed: search value 22, computed from a0=21 + a context global=1 at
  EE address `gp-0x4da4`) — but both existing slots still hold **stale tags
  (47, 46) from the car-select/preview context** and never get reassigned
  for roughly 15-20 simulated seconds.
- During that window, the per-tick processing function
  (`0x2C5A78` in the dongle-loaded EE code, confirmed still entered
  every tick without interruption via a temporary counter hook) keeps
  failing this tag lookup (`0x2C5990`) and returning early — the
  counter at `+0x48` freezes at exactly half its threshold
  (64/128) instead of advancing, and the "issue next read" routine
  (`0x2C6A50`) is never reached.
- The audio actually heard during this window is clean (no clipping, no
  sample discontinuities) — several distinct attack-decay bursts, ~1-3s
  each, matching normal ADSR shapes. Play!'s ADSR release-envelope math
  (`Iop_SpuBase.cpp` `GetAdsrDelta`/status-machine code, called once per
  audio sample as real hardware does) matches the standard, well-documented
  PS1/PS2 SPU formula and shows no obvious bug.
- **Tested and disproven (2026-07-24): these are not lingering voices from
  before the transition — something keeps actively re-triggering new
  key-ons tagged to the stale context throughout the whole ~15-20s window.**
  Added a one-shot `CSpuBase::SendKeyOff(0xFFFFFF)` on both SPU cores
  (`play_core.cpp`'s existing `bgm_connection` BGM-change handler,
  triggered exactly once per genuine transition) to force any voices still
  ringing out at the moment of the BGM change into release. Live A/B test:
  **byte-identical SPU dump with and without the key-off** — same burst
  timestamps, same peaks, to the sample. This proves the bursts are new
  key-on events happening *after* the transition, not carryover that a
  key-off could clear. Reverted (zero benefit, unnecessary risk of an
  unproven behavioral change). Whatever is repeatedly re-triggering
  tag-46/47 content on its own schedule during this window has not been
  located — it is a separate mechanism from the counter/tag-lookup one
  above, most likely a periodic timer/loop (e.g. an attract-mode-style car
  preview cycle) that should have been cancelled on entering the race but
  wasn't.
- **Also tested and disproven: the test's own scripted input was not the
  cause.** `CSpuBase::SendKeyOn`'s existing per-key-on trace (already built,
  `Iop_SpuBase.cpp`, gated on `WHITTYARCADE_TRACE_AUDIO`, prints address/
  pitch/ADSR/a raw sample-data preview) shows the bursts are `core=0`
  channels playing genuine, valid PCM data (ADPCM-header bytes all
  plausible, ~90% nonzero data windows) at addresses that repeat exactly
  (`0x87ee0`, `0x7f540`, `0x7a150`, `0x86970`, `0x84fa0`, `0x856d0`) with
  pitch that changes each retrigger (e.g. `0x0749` to `0x0958` to `0x0ab6`
  to `0x0d7f`...) -- this really does look like the genuine engine-sound
  layer retriggering with changing pitch as if RPM were climbing, not
  leftover/stale content. (Separately, `core=1` channels seen in the same
  window at address `0xFFFF0` with an all-zero 1024-byte sample window are
  inert/silent placeholder voices, not part of the audible symptom either
  way.) The test's input script also had a real confound: it toggled
  `shift_up` on/off in a 5-second loop starting exactly when gas engages,
  which is not how a real player drives and could itself re-trigger a
  gear-change sound repeatedly. Fixed to pulse `shift_up` briefly a few
  times instead of looping it -- the burst pattern was byte-for-byte
  identical before and after this fix, ruling it out as the cause.
- **Net effect of this whole sub-investigation:** the specific mechanism
  producing the perceived "random sfx" is still not pinned down, but two
  working theories (stale lingering voices; the test's own artificial
  input) are now conclusively ruled out via live A/B tests, not just
  reasoning. What's left standing: either (a) this genuinely is the
  race-start rev/countdown sequence -- discrete engine-sound bursts with
  gaps, changing pitch, real data, no clipping -- and it only sounds wrong
  because it's discrete/choppy rather than a smooth continuous rev (a
  cadence/timing issue in whatever re-triggers these voices, not a
  corruption bug), or (b) something about it (wrong pitch curve, wrong
  specific sample choice, timing relative to on-screen action) is
  perceptibly wrong in a way that needs a human ear and/or real-hardware
  reference audio to identify -- neither of which static analysis can
  supply.

### Mechanism decoded (2026-07-24, second pass): the race engine-audio STREAM
stalls at exactly half its first double-buffer — the frame-rate dip below is
a co-symptom, not the cause

Deeper EE disassembly plus a fresh read of existing traces corrected several
earlier conclusions. The full decoded mechanism of RRV's disc-audio
streaming client (all addresses EE, from the licensed dongle code):

- Stream slots: THREE 0x50-byte slots at `0x01EB6E00/0x01EB6E50/0x01EB6EA0`
  (earlier traces only ever watched the first two — slot 2 exists and is
  active). Fields: `+0x00` active, `+0x01` tag, `+0x28` active2 (stream has
  a data source), `+0x30/+0x34` start sector/sector count, `+0x48/+0x4A`
  tick counter/threshold (128), `+0x4D` buffer-half flag.
- Slot open (`0x2c6550`): tag = `epoch*24 + 23 - slotIndex`, where epoch is
  the global at `gp-0x4da4` (written in exactly one place, `0x2c6358`, the
  subsystem init at `0x2c6310` — value is an init argument, currently 1).
- The init function registers EE-side command handlers via `0x2f6378`
  (channel 1): commands `0x200`, `0x203`, `0x8300→0x2c6128`,
  `0x8400→0x2c5a78`, `0x8500→0x2c6048`. Command `0x8400` is the per-stream
  "refill tick" sent by the IOP sound driver (`rspu2_driver`, a REAL module
  the game loads and runs); its payload carries the stream handle
  (`handle = tag - epoch*24`).
- The `0x8400` handler services the slot matching the incoming handle:
  increments its counter (wraps at threshold 128), toggles the buffer-half
  flag at exactly counter==64 (=threshold/2), and at the half/full points
  calls the refill routine (`0x2c6a50`) which — after a gate check
  (`0x2c1998`, global at `gp-0x4db8`, always 0=pass in traces) and two
  SIF-status checks (`0x2f30b0(1)` must return 2, matching CAcCdvd's SIF
  method 0x02 "must be 2" semantics) — issues the next 16-sector disc read.

**Observed facts against that model (all from existing trace logs):**
- The 630 sampled `0x8400` ticks across an entire run — menus AND race —
  ALL carry handle 21 = slot 2, which is open but idle (no data source), so
  its tick is a no-op. Each real stream's ticks come from its own IOP-side
  counterpart; slot 0 (the race stream, handle 23, sectors
  `0x4D03..+0x1D54`) received exactly 64 ticks (a burst the 1-in-30
  sampling missed) and then its ticks STOPPED — counter frozen at 64,
  buffer-half flag mid-toggle, refill for the second half-buffer never
  requested, only the first 16-sector chunk ever read from disc.
- Meanwhile SPU2 core 1 keeps DMA-ing the rotating buffer continuously
  (~75264 bytes/sec-window) but only ~882 bytes of it are nonzero — the
  IOP-side staging buffer is never refilled, so the SPU loops mostly-zero
  data: audibly a "whoosh", exactly matching the user's description
  ("engine revs are whoosh not car noises", "steam train not a car" =
  discrete real key-ons from the sample-based layer over a near-silent
  stream bed).
- The stall NEVER resolves at the stream level: slot 0 stays counter=64
  through the end of every captured run, even after the audio subjectively
  recovers — the game most likely abandons the stream and the resident
  sample-based engine layer carries the sound alone from then on. (Earlier
  "it recovers after ~20s" reads of the amplitude data were about the
  mixed output, not the stream.)
- Menu-era streams (slots with `flag4d=0`, single-buffer mode) always
  showed counter=0 and worked; the race stream is the only user of the
  `flag4d` double-buffer mode observed.

**Full decoded cvd-client protocol (2026-07-24 late session; supersedes the
SPU-IRQ hypothesis below, which was implemented and disproven live):**
- EE "cvd" RPC client struct at `0x01EFBE40`: `+0x04`=mode(2), `+0x0C`=lock,
  `+0x10/+0x14`=waiter list, `+0x18`=pendingMethod, `+0x1C`=workerId(3),
  `+0x20`=completion callback (`0x2c56b0`), `+0x30..0x3F`=last request
  params, `+0x40`=recv buffer (`+8`=cached recv[0], submit returns recv[1]).
  Submit=`0x2f9a18` (sceSifCallRpc at `0x310a78`, call site `0x2f9a74`);
  read request=`0x2fa060` (method 0x0A) / `0x2fa150` (method 9); status
  check=`0x2f9e58` (method 2, returns ret[1] — Play!'s `ret[0x01]=2` is
  correct); `0x2f30b0` wraps it with an alternate no-RPC branch returning
  2 or 6 when `0x2f2c20()` is nonzero.
- RPC method shape per read: method 2 (status, must return 2) then method
  10/9 (read). Method 5 = completion poll used only when a read reply's
  word 0 is nonzero: **tested live — setting `ret[0x00]=2` in CAcCdvd
  method 0x0A makes the game treat every read as async-busy and
  poll method 5 forever (24k RPCs, total silence). Word 0 = 0 (Play!'s
  current behavior) is CORRECT for reads; reverted.**
- Stream refill engine: `0x2c6a50` = "issue read for buffer half a3":
  marks `bufState[a3]=-2` (globals `gp-0x5840`/`gp-0x583c`), alignment
  check ("Cd Stream Alignment not 64 bute!" @0x3DEAF8), gate `0x2c1998`
  (global `gp-0x4db8`==0 → pass; hook shows always 0), status checks
  (must ==2), then submits; on accept `bufState=-1` ("in flight");
  completion callback `0x2c56b0(1)` (invoked from worker ra=0x2f9938)
  flips -1→1 ("ready"). Tick handler only advances a slot's counter when
  `bufState[flag4d]==1`.
- **Where it actually wedges (one step deeper than all previous theories):
  after the race stream's FIRST read completes (bufA=1), the refill
  function `0x2c6a50` is never entered again** — gate-hook hits stop at
  the open (#5), no further method-2/10 RPCs, `bufA` later shows -2 and
  `bufB` -1 in the stuck dump (run-to-run variation in how far it gets),
  counter frozen at 64=threshold/2 on BOTH double-buffer slots (slot 1 is
  open with marker=-1/no data; its 64-freeze is benign). The remaining
  unknown is why the counter==64 transition's advance path does not call
  `0x2c6a50` for slot 0. **Probed 2026-07-24: hooks on both refill
  callsites (`0x2c5d48`/`0x2c5de4`, JALs to 0x2c6a50) fired ZERO times in
  a full run — even while the counter climbed 0→64.** So the static
  reading of handler `0x2c5a78`'s branch structure is wrong somewhere
  (R5900 branch-likely delay slots + undecoded 3-operand MULT words make
  the flow treacherous to read); the actual increment path (the
  `lhu/addiu/sh +0x48` at `0x2c5e3c`) is reached by some route other than
  the assumed advance block, and the refill trigger lives elsewhere. Next
  probe for the following session: hook the increment itself (`0x2c5e3c`)
  and log $ra/register state to discover the true calling context, and/or
  single-step the handler by hooking every branch in `0x2c5b7c..0x2c5e98`
  for one tick around the 64-transition. All hooks are in
  `MA_MIPSIV.cpp` (unconditional prints; JAL/BEQ/ADDIU emitters keyed on
  hardcoded EE PCs) and each rebuild+run cycle takes ~5 minutes with
  `system246_rrv_stream_integration_test` at 6600 frames.
- **Resolved statically right after (no rebuild): the branch-structure
  misreading found.** Handler `0x2c5a78` is a LOOP over 24 iterations
  (`0x2c5ff0-0x2c6000`: loop counter at `sp+0x14`, `slti 0x18`, back-edge
  to `0x2c5ac8`), and its argument (the traced constant "21"/"22"/"23") is
  a **bitmask tested bit-by-bit per iteration** (`0x2c5ac8`:
  `srav v0, arg, i; andi 1; beql 0 → next i`), NOT a stream handle.
  Masks 21/22/23 = 0b10101/0b10110/0b10111 (bits 0-4 = per-stream SPU
  channel groups, rotating). Everything earlier that treated "handle 21 =
  slot 2" is wrong; the tag lookup `0x2c5990` is called per SET BIT with a
  different argument than assumed. The two "refill callsites"
  (`0x2c5d48`/`0x2c5de4`) sit inside a per-bit branch whose entry
  conditions must now be re-derived against the loop model — with the
  correct model, the "counter increments 0→64 then freezes when
  bufState[flag4d] flips to a non-ready buffer" mechanism still stands
  (that part was verified from live data, not statics), and the missing
  event is still "nothing ever refills the second buffer half". Next
  session: hook `0x2c5e3c` (the increment, LHU emitter) logging the loop
  index at `sp+0x14`, the bit mask, and `$a1`, to map which per-bit path
  actually increments; then find the true refill trigger from there.
- **Probed (2026-07-24, final cycle): the `0x2c5e3c` increment NEVER
  executes either — zero hits across a full run — so handler `0x2c5a78`
  maintains no counters at all in practice.** The real `+0x48` writers per
  the existing disassembly grep are `sh →+0x48` at **`0x2c6f20`** and
  **`0x2c6fc8`** (plus `sh →+0x4a` at +4 bytes after each, i.e. they
  write counter AND threshold together — possibly (re)initializers — and
  a reader `lw +0x48` at `0x2c7b30`), all in the never-explored region
  `0x2c6e00-0x2c7c40` of `stream_func_disasm.txt` (already disassembled
  in the session scratch dir; regenerate from any EE dump with capstone
  if lost). START THE NEXT SESSION THERE: disassemble/read that region,
  hook those two SH sites (SH emitter, hardcoded PCs, same pattern as the
  existing hooks in `MA_MIPSIV.cpp`), log `$ra` + slot base + written
  value, and the true tick/refill state machine will be on screen within
  one ~5-minute run. All other instrumentation (cvd RPC call/fail hooks,
  gate hook, completion-callback hook, per-tick mask logging, stream
  struct dumps via `WHITTYARCADE_TRACE_SPUSIF`) is already in place and
  prints in the same log.
- Handle ticks (cmd 0x8400) carry handles 21/22/23 in strict rotation
  continuously (~4.8/frame each, never stop; handle=tag-epoch*24, slot
  tag=epoch*24+23-slotIndex, epoch global `gp-0x4da4` value 1, slots at
  `0x01EB6E00/50/A0`).

**Implemented & kept (hardware-correct, but did NOT fix this bug):**
SPU write-side IRQ (`CSpuIrqWatcher::CheckIrqRange`, start-exclusive/
end-inclusive, called from `CSpuBase::ReceiveDma` read+write paths and
`WriteWord`) and non-lossy IRQ delivery (keep watcher pending while an
unacknowledged IRQ exists instead of unconditionally clearing per render
batch) in `Iop_SpuBase.cpp/.h`. Matches psx-spx-documented hardware
behavior and PCSX2 (behavioral reference only); verified no regression on
the full system246 suite. The old superseded hypothesis text follows for
history:

**Superseded hypothesis (disproven live):** the IOP-side `rspu2_driver` paces
each stream's `0x8400` ticks off the SPU2 IRQ (core 1 IRQ address `0x5410`
is set during the race stream; voices use the classic dummy-start
`0xFFFF0` + rotating repeat-address idiom). If Play!'s SPU IRQ emulation
misses the address-hit conditions this idiom depends on (per-voice read
pointer crossing the IRQ address, and/or DMA/transfer writes crossing it,
and/or cross-core checks), the driver's tick source dies after the first
half-buffer — producing exactly a counter frozen at threshold/2. Research
agents were dispatched (2026-07-24) to (a) mine Play! upstream and
community knowledge for known SPU-IRQ/streaming gaps and (b) compare
PCSX2/pcsx2x6's SPU2 IRQ-raising semantics against Play!'s
`CSpuIrqWatcher`/`CSampleReader` as a reference (GPL: behavior study only,
no code copying). Their findings should identify the precise missing IRQ
condition; the fix would then be implementing that condition in Play!'s
`Iop_SpuBase` sample/transfer path.

### Earlier finding, now demoted to co-symptom: frame-rate dip during the
race-start scene transition

Direct feedback narrowed it precisely: "acceleration sounds like a steam
train not a car" -- i.e. distinct choppy revs, not static/corruption. That
plus the engine-sample analysis above (real PCM data, correct pitch
progression, just gapped) pointed at a **cadence** problem, not a content or
protocol one. Added periodic instrumentation to the integration test
(`video_bridge->metrics().produced_frames` sampled every 60 simulated frames
to compute actual wall-clock core FPS, kept in the test file alongside a
periodic dump of Play!'s existing per-zone profiler stats) and found the
smoking gun directly:

- **Core FPS drops from a steady ~60 to ~33-50 for almost exactly the same
  window as the audio symptom** (measured frame ~4980 to ~6120, about 19
  simulated seconds), recovering to a steady ~60 right as the audio also
  becomes clean and continuous. This is the same window as everything
  documented above (BGM-command-8/"GO!" transition through to the
  44s-sustained-clean-audio run).
- Enabled Play!'s existing (previously-inert; needs `-DPROFILE=1`) zone
  profiler (`Profiler.h`/`.cpp`, zones EE/IOP/SPU/GSSYNC/OTHER/GIF/VIF0/VU/
  VIF1) and sampled it the same way. **VU (vector unit) zone time is
  consistently the dominant nonzero figure during exactly this window**
  (multiple milliseconds per ~1s sample, vs near-zero for every other zone
  including GSSYNC) -- strongly implicating VU (geometry/lighting
  transform) processing for the newly-loading race track/car scene as the
  performance bottleneck dragging the whole frame down.
- **Tested and disproven: `apply_rrv_patches`'s `SetExtraClamping(true)`
  (the pcsx2x6-derived VU clamp-mode-2 fix for road/car vertex packet
  correctness) is not the cause.** Temporarily set to `false` and re-ran the
  identical scenario: the FPS dip pattern was essentially unchanged (same
  magnitude, same window). Reverted immediately (this flag is required for
  correct rendering; disabling it was diagnostic-only, never committed).
- **Status of this finding (superseded as root cause):** the VU-time spike
  and FPS dip are real and worth optimizing eventually, but the stream-stall
  mechanism decoded above is the actual cause of the wrong audio content —
  a 40-50fps dip cannot turn engine samples into near-silence/whoosh, and
  the dip window only partially overlaps the stalled-stream window. Keep
  this data for a future performance pass; do not treat it as the audio
  bug's cause.
- Ruled out: this session's real-driver-chain work (`CAcAtaAtapi`,
  `CAcLoCore`, the dongle module loader) has zero effect either way,
  confirmed via `WHITTYARCADE_DISABLE_REAL_DRIVER_CHAIN=1` A/B testing —
  those modules never become functionally active (see the aclocore-framework
  finding above), so they can't be causing or fixing this. Also ruled out:
  the pre-existing `0x01EFBE40` "ACCDVD client/worker" cooperative-scheduler
  trace (an EE-side game-implemented coroutine system, unrelated to Play!'s
  own IOP thread scheduler) — it does cycle continuously before the race
  starts but stops once racing begins and doesn't reappear in the traced
  window; most likely a separate, unrelated menu-time status-poll worker
  with nothing left to do, not the mechanism gating the stream-tag
  reassignment.

**Not yet found:** the specific trigger that finally reassigns a slot's tag
~15-20 simulated seconds later, or definitive proof of whether the delay is
a genuine Play!-emulation timing bug (e.g. in the still-unidentified code
path that's supposed to force-stop/free the old voices on scene transition)
versus an inherent, if unfortunate, real-hardware-matching characteristic of
this specific game. Resolving that needs either interactive EE-side
breakpoint/single-step debugging (not available in this session) or several
more rounds of the same blind hardcoded-PC hook-and-rebuild cycle used to
get this far — each cycle costs a full rebuild + multi-minute live run.

**Diagnostic infrastructure added this session, kept in place (unconditional,
matching the existing `TraceRrvAcCdvdClientState` convention) in
`third_party/play/Source/MA_MIPSIV.cpp`:** `TraceRrvStreamGateCheck` (hooked
at EE PC `0x2C6AB4`), `TraceRrvStreamProcessTick` (`0x2C3230`, turned out to
be an unrelated cleanup pass), `TraceRrvStreamFuncEntry` (`0x2C5AE4`, the
useful one — prints the search tag, context value, and both slots' tags).
`tests/system246_rrv_stream_integration_test.cpp` still supports
`RRV_TEST_SCREENSHOT_DIR` (BMP capture, no image-codec dependency) for
visual ground-truth checks. An `system246_play_core::dump_ee_ram()` method
existed briefly this session for EE RAM forensics (capstone MIPS64 static
analysis of the live dongle code) but was removed; recreate it the same way
if this investigation resumes — a `std::fwrite` of `vm->m_ee->m_ram` guarded
by the `PS2::EE_RAM_SIZE` constant.

## Principal risks

- Play!'s complete transitive dependency/license audit is not yet signed off.
- Offscreen GS integration and Vulkan context/thread ownership are the main
  engineering spike; the external Play! window test does not solve them.
- RRV relies on three executable patches and HLE for JVS/AC devices, so an
  upstream change can regress boot even when the ROM set remains correct.
- Steering feedback behavior is not yet emulated as a host haptic device.
- A playable title screen is not a performance, audio or complete-race
  certification.
- System 256 games add HDD/DVD and per-title device differences; they require
  their own evidence and test assets.
