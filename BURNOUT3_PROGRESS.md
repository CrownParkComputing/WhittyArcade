# Burnout 3: Takedown native recompilation progress

**Updated:** 3 August 2026  
**Primary milestone:** render the retail XBE intro and retail in-game/frontend menus, with FMV sound.

## Current status

The retail frontend flow now runs **from boot movies through the title screen
into the post-FMV retail screen** (`0x0055C574`), with zero unresolved
indirect calls on that path and the frontend resource registry
(`0x007397B0`, built by `0x0019AE10`) reaching its ready state 23. The
screen's Criterion UI node graph walks ~117 nodes per frame and submits real
draws through DXVK. What is still missing for a recognisable menu frame is
the **element content population**: the menu chrome nodes carry zero colour
words and empty sprite sources (`src index 0, dim 0x0`), so their draws are
invisible against the movie-quad background. This is now a data-flow
question, not missing code — no dispatch drops remain on the path. It must
not yet be described as a working menu.

The handcrafted renderer in `Burnout3Recomp/src/game/fe_menu.c` is a portable
fallback and media/audio integration harness only. It is not the intended menu
implementation and does not count toward the retail-frontend milestone.

## Completed work

### Original XBE execution

- Loads `game_data/xbox/burnout3/default.xbe` into a 64 MB Xbox address space.
- Correctly decodes full xboxkrnl ordinal values instead of treating thunk slots
  or ordinal low bytes as API identities.
- Maps all 147 imported kernel thunks and the required data exports.
- Uses correct stdcall argument cleanup for the imported functions reached so far.
- Implements guest heap/pool allocation, contiguous allocation, file/volume
  basics, ANSI strings, critical sections, events, fake thread objects, object
  references, and thread-priority calls.
- Defers the XBE worker at `0x001D1818` because recompiled CPU registers are
  process-global; running that worker synchronously would corrupt the caller.
- Resolves indirect RenderWare/CRT calls through the original Xbox addresses.
- Restores scanner-missed RenderWare device jump-table branches, standards-table
  setup, point transforms, plugin constructors/destructors, and resource accessors.
- Restores the original world, world-sector, mesh, light, raster, material, and
  skin pipeline constructors that precede frontend resource loading.
- The former bogus `0x92880D8B` (2.45 GB) allocation is gone; it was instruction
  bytes from standard callback `0x001E52C0` being consumed before the required
  pipeline state was established, not a genuine asset allocation.
- Bypasses the Xbox hardware audio graph only; host-native decoding owns sound.
- Repaired the translator's split epilogues at `0x1CE15`, `0x1CE17`, and
  `0x1CE18`. They are labels inside `0x1CBC0`, not callable functions. This
  removed the invalid indirect calls to `1`, `3`, and `4` and stopped `g_esp`
  corruption after `PrgData.bin`.
- Preserves the original callee-saved constants used to write init states 1 and
  7; translated constructors were leaking their internal ESI/EDI values.
- Removes the repeated `0x2F650` loading/logo render pass in fast-menu mode
  while continuing to call the original `0x15F10` initializer.

Last full pre-FMV-render measurement (before fast-menu suppression):

- stage: `pre-FMV geometry (loading/boot screen)`
- 35,457 draw submissions and 543,674 primitives in the latest test
- visible sampled geometry, but still not a correct intro/menu frame
- `XMV_Play` count remains zero

### FMV and Android-ready media

`tools/burnout3_media_import.py` provides a repeatable asset-build conversion.
Criterion and RenderWare logo movies are not part of the current milestone; the
converter is retained for media that the retail XBE actually requests, including
the frontend FMV background:

- parses version-3 `ovid/movie.xwb` without an XACT runtime plugin;
- extracts all 34 bounded ASF/WMA entries;
- converts the entries to AAC/M4A;
- combines the known boot XMV video/audio pairs as H.264/AAC MP4;
- writes a manifest containing offsets, lengths, hashes, and mappings.

Known boot mappings:

| Retail video | `movie.xwb` entry | Portable output |
|---|---:|---|
| `cri_rw30.xmv` | 7 | `movies/cri_rw30.mp4` |
| `Titles30.xmv` | 11 | `movies/Titles30.mp4` |
| `englis30.xmv` | 33 | `movies/englis30.mp4` |

Generated assets are under `build/generated/burnout3_media` by default. The MP4
files contain H.264 video and AAC stereo audio and can be consumed directly by
Android platform media APIs, avoiding an Xbox codec/plugin layer on Android.

### Native FMV sound path

- `manx_fmv` opens video plus optional container audio.
- Audio is decoded and resampled to signed 16-bit, 48 kHz stereo PCM.
- A lock-free producer/consumer ring feeds SDL's audio stream.
- Movie replacement/skip is protected against the audio callback thread.
- `burnout3_audio_callback` now pulls actual movie PCM rather than a test tone.
- MANX creates and owns an SDL3 playback stream for the session.
- The portable fallback prefers converted MP4s through `B3_PORTABLE_MEDIA` or
  `<game>/portable/movies`, then falls back to video-only retail XMV files.

The decoder/audio path has been verified independently. This does not count as
frontend progress until playback is requested by the retail XBE itself.

## Dependency policy

- No runtime XACT/XWB plugin loader is required.
- Asset conversion uses the `ffmpeg` command-line program at build/import time.
- Desktop native decoding links the required FFmpeg libraries directly, including
  `libswresample`; it does not discover per-game codec plugins.
- Android packages should ship the generated standard MP4/M4A assets and use the
  Android media stack where possible.
- Native builds do not start Xbox Live, XONLINE, XNet, Winsock, or their polling
  thread. Local profiles, save/load, and the offline frontend remain in scope.

## Current blockers on the retail path

1. **Menu element content is empty.** The post-FMV screen's node graph walks
   and draws, but the per-node data colour words and sprite sources
   (`src={index,dim}`) are mostly zero, so its geometry is invisible. The
   population step that copies screen content (labels, sprites, colours,
   anchor positions) into the nodes has not been located yet; no indirect
   calls drop on the path, so this is a data initializer or a
   resource-driven fill that has not run, not missing code. Update 2026-08-04:
   since the art-pointer fix below, two 80x37 textured chrome nodes now carry
   real sprite sources (`src index 8`, art object `0x010DF980`), but all
   colour words and the remaining sources are still zero.
2. UI textures beyond the movie surface still bind NULL (fonts/sprites), and
   render/texture-stage state calls remain recording stubs.
3. The decoded `manx_fmv` frame still needs to reach the retail `0x001C59A0`
   movie-quad draw in the Standalone (mechanism exists end to end; not yet
   verified visually).
4. Scanner-missed standard callback `0x001DCB70` and remaining plugin-specific
   state behind temporary identity callbacks need exact translations.

### Superseded 2026-08-03 (previously listed here)

- ~~The four Criterion UI builders resolve to no-ops~~ — all four are lifted
  and registered; the whole menu-screen callback set now resolves.
- ~~The screen never advances past its readiness stage~~ — kind-0 (menu)
  readiness is bridged natively; see the milestone below.

### Offline-only runtime milestone (2026-08-02)

- Replaced `0x000FC5D0`'s Xbox Live/XNet state machine with its own retail
  non-network finalisation path.
- Removed active Xbox Live connection, socket creation, and XONLINE polling
  thread entry points from the native path.
- Verified before the final-construction work that retail init advanced from
  state 9 to state 10 without creating the `0x0021C830` network worker.
- A live gdb stack after the subsequent fixes reached the post-state-14 setup at
  `0x00063590`, proving the former online wait is no longer the first blocker.

### Retail resource-worker milestone (2026-08-02)

- Implemented native `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`,
  and `NtSetInformationFile` handling, forming the base for later save/load.
- Reproduced the retail resource slot's two-phase open/read state machine.
- Preserved the special queued-open path long enough for the cooperative worker
  to open it before the XBE overwrites the inline buffer with read length and
  destination fields.
- Added case-insensitive Xbox path resolution for case-sensitive Linux and
  Android filesystems.
- Verified six original disc resources are read into the XBE's requested guest
  addresses. The earlier `0xFFFFFFFF` handle is no longer treated as a failed
  file read.
- `burnout3_intro_test`, `MANX`, and the native recompilation library
  build successfully.
- Captured frame is still pre-FMV boot geometry. The original full menu, its FMV
  background, navigation, and save/load UI are **not yet visible/complete**.

### RenderWare frontend-loop milestone (2026-08-02)

- Kept the retail XBE frontend/state machine and used the portable RenderWare
  draw bridge as the rendering boundary; the handcrafted menu is not treated as
  the target implementation.
- Identified the second `0x001BEFF0` descriptor as an Xbox D3D hardware-cache
  sentinel (`stride=0`, `head=1`). Valid RenderWare pools retain the original
  free-list algorithm; the nonexistent hardware cache is skipped.
- Fixed a translated callee-saved `EBX` leak that compared a clear quit flag
  against `1` and bypassed the frontend loop.
- Verified with gdb that retail initialization reaches state `0x17`, returns,
  and calls the original `0x000165F0` frontend frame with frame state 1.
- Native `burnout3_intro_test` rebuild succeeds after both changes. No full
  menu frame has been captured yet.

### Retail profile/save-screen selection milestone (2026-08-02)

- Replayed the XBE CRT initializer's exact frontend-manager and screen vtable
  writes at `0x00567170`; the retail manager now resolves screen objects with
  their original vtables.
- Corrected the manager enter/leave wrappers' missing-handler stack recovery.
  The previous wrapper restored past its saved `ESI` slot and replaced the
  newly selected screen with zero.
- Compiled the original `0x00067880` profile/save event handler and
  `0x00086F70` screen activation path, plus the closed UI-builder call set
  `0x001C18A0`, `0x000CB5F0`, and `0x000F9C00`.
- Verified the original event sequence reaches event 3 and writes
  `0x00557A70 = 0x0055CB88`. The retail renderer observes the same non-null
  screen pointer, and the frontend UI list is populated.
- The visible full menu was not complete at this checkpoint because
  `0x0055CB88` still had a null vtable. The next milestone below resolves it.

### Retail menu callback and native-audio readiness milestone (2026-08-02)

- Compared the Burnout startup path with FlatOut's selective CRT replay and
  added `tools/analyze_b3_crt.py` to scan Burnout's 13,965 active initializer
  entries without enabling unrelated Xbox libraries.
- Found the exact retail thunk at `0x0026AED0`: it passes object-bank base
  `0x00559390` to constructor `0x00063B80`.
- Translated the complete constructor bank. Its original write at
  `0x00559390 + 0x37F8` restores `0x0055CB88`'s vtable to `0x003AC06C`.
- Translated and permanently enabled the verified retail callback
  `0x000871D0` plus readiness helpers `0x00014FB0` and `0x000151D0`; they do
  not require the unsafe whole-dispatch experiment.
- Verified live execution of `0x000871D0`. The callback's state is 0, global
  frontend state is 5, and the real object address is `0x0055CB88`.
- Removed its dependency on the intentionally absent Xbox DirectSound stream
  pool for frontend kind 2. The host-native audio backend now reports ready,
  changing the callback flags from `0/0` to `0/1`.
- The next exact gate is the render/video flag: `0x000151D0` changes the active
  resource state from 24 to 2, then waits on omitted initializer `0x001C92C0`.
- Native `burnout3_intro_test` rebuild succeeds with the restored DXVK path.
  No complete menu frame has been captured yet.

### Native-video boundary and stable menu loop milestone (2026-08-02)

- Classified `0x001C92C0` and its helpers with the XBE disassembler. The chain
  constructs and polls an XDK XMV decoder and Xbox media packets; it is not the
  Criterion menu layout or navigation implementation.
- Replaced that Xbox-only object with a narrow native-media readiness adapter.
  It preserves the guest-visible state fields while reserving movie decode,
  audio decode, timing, and texture upload for the existing `manx_fmv` backend.
- Verified the retail callback changes from readiness `0/1` to `1/1`, active
  render state changes to 23, and screen `0x0055CB88` advances from state 0 to
  state 1 and then state 2.
- Replaced the broken translated x87 timer constructor at `0x001B5880` with its
  exact native calculation (`performance frequency × milliseconds × 0.001`).
  The old translation lost the x87 value across `__ftol2`, wrote a zero period,
  and crashed in `__alldiv` on the second menu tick.
- The original frontend loop now remains stable for repeated menu frames. It
  reaches retail init state 23, frame state 5, and the original screen pointer,
  but reports clearing-only with zero menu draws because the four Criterion UI
  builders above have not yet been promoted into the native function set.
- No complete menu frame has been captured yet.

### Retail frontend geometry framebuffer milestone (2026-08-02)

- Promoted the original UI render-list callbacks at `0x001C1930`,
  `0x000F9CE0`, and `0x000B53F0`, retaining the Criterion render-list and
  movie-quad builders instead of substituting a host-authored menu.
- Fixed packed SSE lowering used by the RenderWare frontend path and corrected
  the two-argument indirect-callback failure stack contract. The UI list now
  walks only its valid retail nodes; the earlier `0x3F800000` pseudo-link was
  an emulated-stack artefact, not corrupt XBE menu data.
- Used native GDB hardware watchpoints on the mapped guest address space to
  prove the RenderWare root context is constructed with unit scale and then
  identify `0x00063590` as the later writer collapsing it to zero.
- Restored the Xbox display-device contract by publishing the native device's
  `640x480` dimensions at `0x00557870` and `0x005592C8`. The original code now
  resolves the root scale to `{1.0, 0.933333}`, matching the observed Xbox
  frontend transform.
- Replayed the exact skipped scalar CRT initializers at `0x002A2340` and
  `0x002A2360`, copying the XBE's literal `640.0` and `480.0` frontend design
  dimensions into `0x0056FD58` and `0x0056FDE8`.
- Extended the native XMV boundary to publish the decoded surface dimensions
  (`640x480`) expected by the retail movie builder. This removes the previous
  NaNs without restoring the Xbox XMV decoder or adding a codec plugin.
- Verified the original frontend now constructs and submits a finite
  `640x448` movie/menu quad. `burnout3_intro_test` passes with 7,255 draws,
  14,526 primitives, and 5,120 sampled framebuffer pixels covered.
- Captured the first retail-geometry framebuffer at
  `artifacts/burnout3_retail_menu_geometry.png`. It is currently a flat grey
  movie surface above the original black overscan band: geometry is visible,
  but the native decoded FMV frame is not yet uploaded/bound and the complete
  menu text/navigation layer is not yet present.

### Retail menu-screen flow milestone (2026-08-03)

- Fixed a lifter truncation in menu sprite emitter `0x001C7C90`: the function
  database ended it at `0x001C7EAA`, dropping its vertex-count commit
  (`mov [0x4A1B9C], ecx`) and emit-loop back-edge. Corrected to `0x001C7EB2`
  and re-lifted; any batch appended to a non-empty vertex buffer previously
  emitted nothing.
- `burnout3_intro_test` gained an env-gated `B3_FMV_DONE` stage machine that
  stands in for the host movie player and the player's Start press, driving
  the retail flow headlessly: boot movie finished → title/attract
  (re-activation with id `0xF90`) → Start → menu screen request.
- Traced and translated the frontend flow-manager chain: `0x0006C280` (menu
  flow entry; key `20FEDE85:944140D3`), `0x0006C210` (next stage; its true
  extent `0x6C210–0x6C279` was split by the scanner at `0x6C25F`, hiding the
  enter branch that registers retail screen `0x0055C574` via `0x0008D790`),
  per-frame screen callback `0x0008DA30` (scanner-missed, added to the
  database at `0x8DA30–0x8DC57`), plus content builders `0x000C7FF0`,
  `0x000FA840`, `0x001C2080`, `0x001C51A0` and ~35 transitively required
  functions until the link closed.
- Bridged kind-0 (main menu) readiness in `sub_00014FB0`: every kind-0 path
  polls an XDK audio stream (menu music) on the removed Xbox DirectSound
  graph via `0x001CBF30`/`0x001CE140`; the host backend owns music, so that
  hardware-only dependency now reports ready, exactly like the existing
  kind-2 bridge.
- **The frontend resource registry now builds**: `0x0019AE10` completes into
  `0x007397B0` (ptr `0x0159F000`, state 3 → 22 → 23, result 1) — it had been
  stuck at state 1 since the first retail-frontend runs. One of the
  transitively lifted resource-family functions was the missing link in its
  async parse chain.
- The DXVK stride-24 converter now carries the real packed vertex colour
  (previously overwritten with white), and draws without a texture use
  vertex diffuse whenever the batch carries any non-zero colour, keeping the
  TEXTUREFACTOR grey only for genuinely colourless bring-up geometry.
- End state: active screen `0x0055C574`, UI list walking 117+ nodes/frame,
  **zero `[DISPATCH] ??` lines across a full run**. `burnout3_intro_test`
  passes throughout; the ratchet was never lowered.
- Correction to the build/verify section below: run the intro test **without**
  `B3_DISPATCH=1` — the whole-table experiment stalls the boot and is
  deliberately off by default (see `b3_native_dispatch.c`); the verified menu
  chain resolves through `recomp_lookup_manual` unconditionally.

### Live-run input/freeze fixes (2026-08-03, second session)

- Deleted the handcrafted fe_menu standalone permanently: the old
  `Burnout3Standalone` target and `src/burnout3_standalone_main.cpp` are
  gone; the retail XBE frontend (formerly `Burnout3StandaloneDXVK`) now owns
  the `Burnout3Standalone` name. The fake menu can no longer be launched.
- Live-run freeze root-caused: the standalone's Enter handler poked guest
  byte `0x4A1C75` believing it was "A/Start". `sub_00013F10` shows
  `0x4A1C74..79` are directional one-shots (74/75 = dpad/stick UP,
  78/79 = dpad/stick DOWN, 76/77 = left/right); writing 75 flips the
  frontend nav block at `0x4A4B90` to backward, and `sub_000636D0` gates
  its whole flow machine off while `[0x4A4B90]==0` — a permanent freeze.
  Start/A/B already reach the game correctly through the pad-button path
  (`g_xinput_buttons` → `XInputGetState`), so the latch pokes for
  Enter/Escape are removed; arrows keep the directional latches.
- The title attract movie now loops: skipping the boot reel with Enter no
  longer leaves the menu movie unopened (frozen last frame), and a
  naturally finishing Titles30 pass reopens itself while reporting the
  retail movie-complete byte, matching the retail attract cycle.
- Timing verified along the way: `0x4AE1FC` frame delta updates at
  0.0166667 s per frame live and headless (retail unit is seconds;
  `0x49C120` = 0.016667 from `0x3B1838`), so fades were never the stall.

### Live session result (2026-08-03, late)

Verified live in a window: boot FMV reel plays with audio → title attract
(Titles30 looping behind the retail frontend) → Start via the pad path →
flow walks to the post-FMV menu screen (`0x0055C574` active, per-frame
callback `0x0008DA30` dispatching, nav healthy). Two open issues from the
run:

1. The active menu is INVISIBLE (known empty node-content issue) — to the
   player this reads as "FMV loops, nothing responds". It is actually
   running; content population is the wall.
2. After ~4 minutes the process died with SIGSEGV inside DXVK's
   `DxvkDescriptorCopyWorker::runWorker` (device IS created with
   D3DCREATE_MULTITHREADED; no DXVK err/warn lines precede it). Core dump
   saved: `coredumpctl gdb 780578`. Suspect area: cross-thread movie-frame
   staging vs. draw-thread texture upload, or dxvk-native descriptor heap
   churn from the attract cycle re-binding each pass.

### Content-population session (2026-08-03, third)

- **All 110 skipped CRT scalar initializers replayed** (`f_crt_replay.c`,
  called from `sub_00015A60`): the `0x5578xx` flow constants (fade
  threshold 0.4 s, rates 2.5, colour scale 255.0) and the `0x56FDxx`
  layout/colour pool now hold retail values. The frontend flow settles
  cleanly on the menu screen (`callback=0x55C574`, fade at threshold,
  ramp 1.0) instead of cycling.
- UI draws now run with standard alpha blending (SRCALPHA/INVSRCALPHA);
  previously every translucent element (menu fade veil) landed opaque.
- **The recurring "crash after ~5 minutes" was the shutdown path**: closing
  the window called `b3_env_shutdown()` (guest unmap + offset zeroed) while
  the detached retail game thread still ran — every close produced a
  SIGSEGV core (si_addr `0x2004D5378` before the offset clear, bare
  `0x4D5378` after). The standalone now `_exit(0)`s like the intro test.
  The readback also copies through a system-memory surface (CopyRects)
  instead of locking the live swapchain.
- **Remaining wall for a visible menu, confirmed live**: the frontend art
  dictionary pointer at `0x00464658` is NULL at menu entry (verified via
  gdb at `sub_0006C280`), so every sprite/text node has no source
  (`src={index:0,dim:0,0}`) and the menu renders blank over its veil. The
  dictionary's methods live in `sub_00050A70`; it is written only through
  a computed base (no absolute stores). Next step: find its retail
  constructor/registration on the Frontend.txd path (callers `0x509B0` /
  `0x50F60`) and promote that chain.

### Frontend art-pointer fix (2026-08-04) — "art dictionary NULL" SOLVED

The "frontend art dictionary pointer `0x00464658` NULL at menu entry" wall
turned out to be misdiagnosed: `0x464654..0x4646C8` is a 30-slot table of
name-resolved frontend art objects (FE, BG, B3Logo, buttons, dpad, medals,
HUD signs, …), filled by the retail `sub_00062DA0` loop from dictionary
`[0x004D1FE0]` (the parsed `Global.txd`) at init. Runtime proof (gdb
watchpoints + per-iteration register dumps): all 30 lookups ran, but every
lookup whose name differed in case from the dictionary entry ("BG" vs "bg",
"b3logo" vs "B3Logo", lobby, shutter, cup, LOADCAR*, …) returned NULL —
13 of 30 slots zero. Root cause was a **lifter flag bug**: standalone
`cmp al, imm8` emitted no flag capture, so `_cf` stayed 0 and the CRT
`_stricmp` (`sub_00248FF0`) case-fold idiom `cmp al,0x1A / sbb cl,cl /
and cl,0x20` never folded — retail string compares silently degraded to
case-SENSITIVE. Fixed in `tools/recomp/lifter.py`: whenever sbb/adc is
lifted and the tracked flag-setter is cmp/sub/test, inject the CF
expression (`_cf = ((lhs) < (rhs)) ? 1 : 0`, or `_cf = 0` after test).
All 71 already-lifted sbb/adc-using functions were re-lifted. Result:
all 30 FE art slots populate (verified live at menu; `0x464658` =
`0x01008680` = the "bg" art object), intro-test menu-path geometry
coverage rose 5,120 → 13,440 px (two 80x37 chrome sprites now carry real
art, `src index 8`). Menu screen advances to state 3 live. Method notes:
the write to `0x464658` is computed (`MEM32(ebx) = eax` in `0x62DA0` with
`ebx = arg+0x63C`, arg = frontend manager `0x464018`) so no static xref
exists; offset-into-symbol gdb breakpoints (sym+0xNN) do NOT track guest
addresses — the host code for one guest function spans ~0x200+ bytes;
`x/s` on a guest VA crashes batch gdb — always `x/s 0x200000000+expr`.
Open knock-on: any other previously-lifted function relying on
`cmp reg,imm` → flags beyond sbb/adc (e.g. setcc handled separately)
should be treated as suspect until re-verified.
Also pre-existing, unrelated to this fix: `burnout3_gameplay_unit_test`
fails at `xbox_MemoryLayoutInit` (MANX-side helper), needs its own look.
New env hook: `B3_AUTO_START_MS=<n>` in `burnout3_dxvk_standalone.c`
presses A+Start through the real pad path for 1.5 s at n ms — scripted
attract→menu transition for headless captures (boot reel skip needs the
host-side `B3_SKIP_BOOT_MOVIES=1`).



## Next milestones

1. Upload and bind the `manx_fmv` decoded frame to the retail `0x001C59A0`
   movie-surface draw, keeping video/audio decoding native and synchronized.
2. Promote the next original screen-state callbacks/builders that populate the
   frontend labels, selection highlight, and menu navigation render lists.
3. Ratchet the regression from framebuffer coverage to the first recognisable
   original main-menu capture and exercise controller navigation.
4. Render and navigate the original frontend/in-game menus, then validate local
   profile save/load.
5. Audit subsequent Xbox service boundaries for native replacements: media
   codecs, DirectSound, kernel/file wrappers, save storage and hardware render
   helpers. Keep Criterion gameplay/UI logic; networking remains removed.
6. Remove the handcrafted menu from the normal Burnout 3 launch path once the
   retail path owns the frontend.

## Build and verification

```bash
cmake --build build --target burnout3_media_import -j4
cmake --build build --target burnout3_fmv_test burnout3_intro_test -j4
cmake --build build --target Burnout3Standalone MANX -j4

# Portable media/audio test
./build/burnout3_fmv_test \
  ./build/generated/burnout3_media/movies/cri_rw30.mp4

# Original XBE progression test
B3_DISPATCH=1 ./build/burnout3_intro_test \
  ./game_data/xbox/burnout3/default.xbe
```

## Key files

| File | Purpose |
|---|---|
| `src/xbox/burnout3_kernel_shim.c` | minimal Xbox kernel/data/heap bridge |
| `Burnout3Recomp/src/game/recomp/native/b3_native_dispatch.c` | indirect-call and missed-entry adapters |
| `Burnout3Recomp/src/game/recomp/native/f_0x135040.c` | portable replacement for Xbox audio graph bootstrap |
| `src/fmv/manx_fmv.c` | native timed video/audio decoder |
| `src/xbox/burnout3_session.cpp` | MANX video/input/audio session |
| `tools/burnout3_media_import.py` | XWB extraction and MP4/M4A conversion |
| `tests/burnout3_fmv_test.c` | decoded video/audio integration test |
| `tests/burnout3_intro_test.c` | original-XBE progression regression |
