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
