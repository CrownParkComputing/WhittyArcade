# Namco System 246/256 board plan

Status: research and ROM-backed feasibility validation completed on 2026-07-22.
The first target is **Ridge Racer V: Arcade Battle (RRV3 Ver.A)**, using the
stable MAME short name `rrvac`.

## Decision

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
