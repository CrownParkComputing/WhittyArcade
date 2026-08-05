# Burnout 3 Standalone (MANX)

`Burnout3Standalone` runs the Burnout 3: Takedown session as its own
executable, without the MANX launcher. It is the same session the
launcher runs (`arcade_board_type::xbox`, `src/xbox/burnout3_session.cpp`),
on the same presentation stack, driven by a small dedicated main loop.

```
./build/Burnout3Standalone [game_data_dir]
```

`game_data_dir` defaults to `game_data/xbox/burnout3` relative to the
working directory and must contain `default.xbe` plus the game data
subdirectories (`Data/`, `Tracks/`, `ovid/`, `pveh/`, `sound/`).

## What it is built from

The entry point is `src/burnout3_standalone_main.cpp`: parse the data
directory, `load_settings()`, construct the shared `arcade_video_worker`
and `arcade_cabinet_state`, build the session through the existing
`make_xbox_burnout3_session` factory, then run main.cpp's single-cabinet
loop — settings-change persistence, paused-vs-run_frame, 60 Hz deadline
pacing — minus everything launcher-shaped.

### Modules linked (compiled from the same sources the launcher uses)

| Area | Sources |
| --- | --- |
| Session | `src/xbox/burnout3_session.cpp`, `burnout3_bridge.c`, `burnout3_importer.c` |
| Game code | static lib `burnout3_recomp` (kernel shim, native game layer, frame pump, `vulkan_d3d8.c`, TXD/BGV/AWD/track loaders, `fe_menu.c`, `rw_bridge.c`, `rw_renderer.c`) plus `manx_fmv` for the intro reel |
| Presentation | `arcade_video_worker.cpp`, `arcade_renderer.cpp`, `arcade_presenter.cpp`, the generated SPIR-V shader headers |
| Input | `arcade_input.cpp` (the "daytona" driving mapping), `input_mapping.cpp` |
| Settings | `arcade_settings.cpp` (`settings.ini` load/save) |
| Pulled in by the renderer, dormant here | `arcade_catalog.cpp`, `arcade_game_probe.cpp`, the nine board ROM probes (`system22/system246/xbox360/model1/model2/galaxian/system16b/gng/namco`), `rom_library.cpp`, `native_title_library.cpp`, `bezel_library.cpp`, `network_video_link.cpp`, `stb_image_impl.cpp`, `model2_draw_list.cpp`, `model2_geometry.cpp` |
| Header-only | `arcade_session_internal.h` (`video_emulator_session`), `title_capture.h`, `wall_log.h`, `wall_layout.h`, `twin_window_layout.h`, `platform_paths.h` |

External libraries: SDL3, SDL3_ttf, Vulkan, GLEW/OpenGL, minizip/zlib,
ffmpeg (via `manx_fmv`), shaderc, threads.

### Launcher-only code it excludes

- `src/main.cpp` shell: runtime options, arcade wall companions, linked
  cabinet spawning/ring, netplay status plumbing, tool commands.
- Frontend: `arcade_frontend.cpp`, `launcher_menu.cpp`, `input_mapper.cpp`
  (remap UI), `multiplayer_lobby.cpp`, `banner_library.cpp`,
  `media_library`, `play_stats`, `high_scores`,
  `persistent_data`, platform file dialogs.
- Other boards: `arcade_session.cpp` (the all-boards factory) and every
  machine/audio/session module for System 22, Model 1/2, System 246,
  Galaxian, System 16B, GnG, Namco, Xbox 360 — none of their CPU cores
  (musashi, moira, v60, i960, …) link either.
- Plugins and native titles as processes: `game_plugin_host`,
  `plugin_session`, `plugin_audio`, `xbox360_native_runtime/session`,
  `xbox360_asset_import`.
- `arcade_audio_output`: the burnout3 session has no host audio path yet,
  so nothing references OpenAL.

## Launcher entanglements that block further slimming

These are why the "dormant" row above exists at all; each is a seam worth
cutting if standalone ports become routine:

1. **`arcade_renderer.cpp` owns launcher furniture.** The in-game game
   picker/browser rows and board naming live inside
   `polygon_renderer_gpu`, so the presentation stack drags in
   `arcade_catalog` + `arcade_game_probe` + all nine ROM probe modules +
   `rom_library`/`native_title_library` (and with them minizip and the
   JSON reader) into a build that can never switch games. Splitting the
   picker out of the renderer behind a small interface — or a compile-time
   `MANX_NO_LIBRARY_UI` — would cut the catalog out of standalone ports.
2. **Model 2 frame decode lives in the board-neutral renderer.** The
   present path calls `model2_build_draw_vertices`/`model2_build_color_table`,
   so `model2_draw_list.cpp` + `model2_geometry.cpp` link even though this
   session only ever submits RGBA frames. Moving the decode to the Model 2
   submission side would drop both.
3. **`burnout3_session.cpp` calls `rom_library_path()`** in its
   ISO/RAR auto-import branch, which is the other route into
   `rom_library.cpp`. Passing the import destination in from the caller
   would sever it (the renderer seam above still has to be cut first for
   it to matter).
4. **`arcade_video_worker` knows about netplay.** `present_rgba_frame`
   hashes frames through `arcade_input_netplay_*`, so the video worker
   cannot link without the socket-bearing input module. Harmless here —
   input is wanted anyway — but it is a hidden coupling.
5. **`network_video_link`** (streaming-era picture link) is referenced by
   the renderer and comes along despite netplay-less standalone use.

## Framework promotion candidates

Generic pieces built for Burnout 3 that belong in the MANX
framework layer rather than the port:

- **FMV playback — done.** `include/manx_fmv.h` + `src/fmv/manx_fmv.c`
  (static lib `manx_fmv`, present only when the ffmpeg dev libraries
  are). Wall-clock, pts-paced decode of anything ffmpeg demuxes, with BGRA
  output for D3D8-style texture staging or RGBA for
  `present_rgba_frame`. It replaces the port-private `xmv_player` and the
  Burnout intro reel now plays through it; the container fps is still
  ignored (XMV claims 1000/1) and late frames still drop.
- **`vulkan_d3d8.c` — a D3D8-on-Vulkan backend.** Nothing in it is
  Burnout-specific: FVF layout canonicalization, strip/fan/quad/indexed
  expansion, BC texture upload, fixed-function WVP pipeline. Any D3D8-era
  title recompiled the same way could sit on it; candidate home
  `src/d3d8vk/` with the unit tests (`burnout3_render_unit_test`) renamed
  along.
- **Kernel shim memory + path layer.** `burnout3_kernel_shim.c`'s
  mmap-based 64 MB guest space, `MEM32`-offset scheme and drive-letter
  path translation are what every original-Xbox recomp needs first; a
  second Xbox title would want them verbatim.
- **Pad translation pattern.** The `input_state` → `b3_pad_state` block in
  `burnout3_session.cpp::run_frame` (steering centred 0x800 ± 0x580,
  gas/brake 0–0x610, stick thresholds 0x60/0x90) re-derives the action
  board's value ranges, as each driving session does. A shared helper
  producing a normalized pad (int16 steer, uint8 gas/brake, digital
  d-pad) would make the next port's input a three-line mapping.
- **RenderWare asset loaders.** `txd_loader`/`bgv_loader`/`awd_loader`
  parse stock Criterion formats, not Burnout ones; worth a `criterion/`
  module the day a second RenderWare title lands.

## Boot log contract

`fe_menu.c` prints `[XMV] playing <path>` per intro movie (the line the
smoke checks grep for); `manx_fmv` prints `[FMV] playing <path> (WxH
codec)` with the decoder detail. Both appear once per movie in a healthy
boot.
