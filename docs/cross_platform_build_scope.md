# Cross-platform build scope

Status: Windows UCRT64 packaging and native Linux distribution CI were
implemented in July 2026. Android, AppImage and Flatpak remain scoped future
work; Android is deliberately deferred while the desktop builds settle.

The revised delivery order is:

1. shared platform foundation;
2. Windows x86-64;
3. native Linux distribution coverage, followed by portable packaging;
4. Android ARM64, preserving the current GUI composition, when resumed.

The emulator cores, game manifests and ROM validation remain shared. No
platform package may contain ROMs, firmware, keys or other copyrighted game
data.

## Product contract

- Preserve the launcher's 800 x 720 logical canvas, colours, typography,
  hierarchy and menu order. Platform scaling may letterbox the canvas and
  account for a safe area, but must not redesign it.
- Preserve the compact controls panel's 560 x 510 composition. Android shows
  it as a centred modal on the existing surface instead of opening a second
  native window.
- Keep keyboard, mouse and physical-controller mappings compatible across
  desktop platforms. Touch is an additional Android input source, not a new
  set of game-specific input code.
- Keep ROM import, audit, EEPROM/NVRAM, high scores and controller setup in the
  application on every platform.
- Ship the proprietary licence and all third-party notices beside or inside
  every package. Public release repositories contain packages and
  documentation, never private engine source.

## Current portability audit

| Area | Current implementation | Porting consequence |
|---|---|---|
| Launcher GUI | SDL3/SDL3_ttf at a fixed 800 x 720 logical size | The same layout can be scaled on Android, Windows and Linux. Touch hit-testing and safe-area handling must be added. |
| In-game controls panel | A separate compact SDL window | Android needs the same 560 x 510 panel rendered as a modal on its single app surface. |
| Gameplay renderer | Desktop OpenGL 4.3, GLEW and eleven GLSL 430 shader stages | Works for current Linux and Windows targets. Android needs a GLES or native Vulkan rendering path; changing only the final presenter is insufficient. |
| Alternate presenters | Vulkan and software presentation consume an OpenGL-rendered frame | They do not remove the Android desktop-OpenGL dependency. Vulkan rasterisation is a separate future backend. |
| File selection | `fork`, pipes and `zenity` in `arcade_frontend.cpp` | Replace with a platform file-picker service: Android Storage Access Framework, Windows `IFileDialog`, and XDG portal with a Linux fallback. |
| Persistent paths | `HOME`, `XDG_CONFIG_HOME` and `XDG_DATA_HOME` in several modules | Centralise config, data, cache, ROM-library and asset paths while retaining Linux migration compatibility. |
| Font | Host files under `/usr/share/fonts` | Bundle one redistributable font and load it through SDL RWops so it also works from APK assets. |
| Build dependencies | Required desktop OpenGL/Vulkan/GLEW plus unconditional pkg-config and GNU flags | Use target-based dependencies, platform options and compiler-conditional flags. Make presenters optional. |
| Input | Keyboard, mouse and SDL controller events | Add SDL finger events, logical-coordinate conversion and configurable virtual controls. |
| Lightgun cursor | Coloured SDL system cursor | Retain it on desktop; render the same thick P1/P2 crosshair as an in-game overlay on touch devices. |
| Tests | Several tests include POSIX headers and environment helpers | Add portable temp-directory/environment helpers, then run the non-ROM suite on all desktop targets and Android ARM64. |

Relevant boundaries are visible in
[`launcher_menu.cpp`](../src/launcher_menu.cpp),
[`arcade_frontend.cpp`](../src/arcade_frontend.cpp),
[`arcade_renderer.cpp`](../src/arcade_renderer.cpp), and
[`CMakeLists.txt`](../CMakeLists.txt).

## Workstream 0: shared platform foundation

This work lands before any public Android or Windows package. It avoids three
separate platform forks.

### Build and process structure

- Split the native code into reusable targets: emulation/core libraries, host
  services, renderer, and a thin platform entry point. Desktop keeps an
  executable; Android builds the SDL entry point as a shared native library
  loaded by its Activity.
- Replace global include/link variables with imported CMake targets. Guard GNU
  flags and `libm` by compiler/platform, make Vulkan and GLEW optional where
  unavailable, and put tests behind `BUILD_TESTING`.
- Pin every packaged dependency and record its licence. Linux may continue to
  use system packages for development; release builders must be reproducible.
- Keep the board-neutral session interface unchanged so platform work cannot
  add Android/Windows branches to individual machines.

### Platform services

Introduce narrow interfaces for:

- config, data, cache, ROM-library and bundled-asset paths;
- asynchronous file/folder selection;
- opening a bundled asset as bytes/SDL RWops;
- app lifecycle, audio focus and platform messages;
- one owner for SDL initialisation and the active app surface.

Linux continues to recognise the existing XDG locations. Windows migrates to
an application directory below the user's local app-data folder. Android uses
app-private files for imported ZIPs and settings. A selected Android
`content://` document is streamed or copied into app-owned storage before the
existing `std::filesystem` ROM loaders see it.

### GUI and renderer seams

- Make `launcher_menu` receive its window/render host instead of creating and
  destroying SDL globally.
- Preserve the logical rectangles and render them through a supplied desktop
  window or Android app surface.
- Convert pointer/touch coordinates through SDL's logical-size transform so
  snapshots and hit regions remain identical at 800 x 720.
- Isolate desktop GL headers/context creation from renderer operations. Shader
  sources receive a platform preamble rather than embedding one desktop GLSL
  version everywhere.

### Foundation exit gate

- The existing CachyOS build and all 46 tests remain green.
- Launcher and compact-panel screenshots match the Preview 2 logical canvas.
- Linux still discovers existing settings, ROMs, NVRAM and high scores.
- Headless tests no longer require Android/Windows-incompatible POSIX helpers.

## Workstream 1: Android ARM64

Paid Google Play distribution has additional IP, dependency, privacy, EULA and
store-review gates. See the dated
[Google Play paid-distribution assessment](google_play_sale_feasibility.md);
an Android APK/AAB is not considered sale-ready merely because it builds.

### Target and packaging

- First public target: `arm64-v8a`, landscape phone/tablet, minimum Android 8
  (API 26). Build an `x86_64` variant only for emulator/debug use initially.
- Use a Gradle application with SDL's Android Activity/JNI shell and Android
  Gradle Plugin `externalNativeBuild` calling the NDK CMake toolchain.
- Build with the latest stable SDK/NDK selected when implementation begins,
  and ensure every native library is compatible with 16 KB page-size devices.
- Produce a signed standalone APK for sideload/device testing. Public GitHub
  APK distribution is a business decision: a paid Play package should not also
  be published there as the same free binary. Google Play delivery uses an AAB
  and the additional commercial gates linked above.

SDL documents the required Activity/JNI and shared-library arrangement in its
[Android README](https://wiki.libsdl.org/SDL3/README-android), while Android's
NDK guide documents the supported
[Gradle/CMake toolchain](https://developer.android.com/ndk/guides/cmake).

### Graphics decision and risk gate

The first Android renderer should be a GLES 3.2 compatibility path, not a
rewrite of the rasteriser into Vulkan. The existing renderer uses integer
2D/array textures, framebuffer objects, vertex arrays and conventional
vertex/fragment shaders; these have direct GLES 3.2 equivalents. The port
still needs deliberate work:

- convert GLSL 430 sources to `#version 320 es` with precision declarations;
- replace GLEW and desktop-only context/API details;
- validate integer texture formats, framebuffer/readback paths, depth writes,
  blending and texture limits on both Adreno and Mali;
- disable the desktop alternate-presenter choices in the first Android UI;
- retain a renderer boundary so a native Vulkan rasteriser can be added later.

Android now recommends Vulkan for new engines, but GLES remains supported.
For this existing GL renderer, GLES is the shortest correctness-preserving
first release. A time-boxed device spike must compile every shader and render
captured System 22, Super System 22, Model 1 and Model 2 frames before the rest
of the Android packaging is treated as committed. See Android's current
[graphics guidance](https://developer.android.com/games/develop/vulkan/overview)
and [GLES version support](https://developer.android.com/develop/ui/views/graphics/opengl/about-opengl).

### Dependencies

- Build SDL3, SDL3_ttf, zlib, MiniZip, GLM and libmpg123 for ARM64 from pinned
  sources or verified prebuilts.
- Build OpenAL Soft for Android and retain the existing board audio interface.
  OpenAL Soft officially supports Android and CMake:
  [upstream project](https://github.com/kcat/openal-soft).
- Bundle the chosen font and licence; do not depend on an Android system font
  path.
- Package licence/notices in the APK and expose them from an About/Licences
  menu item.

### ROM import and storage

- Launch `ACTION_OPEN_DOCUMENT` for one or more ZIPs and
  `ACTION_OPEN_DOCUMENT_TREE` for a ROM directory.
- Take persistable URI permission where appropriate, stream selected content,
  and copy supported archives unchanged into the private WhittyArcade ROM
  library. The existing import audit runs after the copy.
- Show progress, cancellation and insufficient-space errors. Never request
  broad "all files" access.
- Keep export/restore operations on the same picker service.

The Android
[Storage Access Framework](https://developer.android.com/training/data-storage/shared/documents-files)
provides user-selected file and directory access without general storage
permission.

### Touch and controller behaviour

All touch controls feed existing logical actions and therefore remain
remappable per game.

| Android gesture/control | Default logical action |
|---|---|
| Tap a launcher row | Select/activate that existing 800 x 720 row |
| Swipe or wheel gesture | Scroll the existing list |
| System Back | Existing Back/close action |
| Touch/drag in a lightgun viewport | Absolute gun aim |
| Quick tap in the lightgun viewport | Aim and fire one shot |
| Hold `PEDAL / ATTACK` | Time Crisis stand/attack state |
| Release `PEDAL / ATTACK` | Time Crisis default hidden/cover/reload state |
| `COVER / RELOAD` button | Force cover/reload, equivalent to desktop right-click |

Time Crisis uses multitouch: one thumb may hold the pedal while the other aims
and fires. Its custom pointer is drawn over the game, never shown as an Android
mouse pointer; P1 remains cyan and P2 remains red/magenta. USB/Bluetooth
controllers and physical mouse input continue through the normal mapping
layer. Other gun games select their own profile without changing the Android
event code.

Driving/twin-stick games receive a configurable translucent virtual-control
layout after touch lightgun input is stable. Virtual controls can be hidden
when a physical controller is active.

### Lifecycle and Android acceptance gate

- Lock the first release to landscape and preserve the logical layout across
  aspect ratios, cut-outs and density changes.
- Pause emulation and release/reacquire audio focus correctly when backgrounded;
  resume without losing settings, EEPROM/NVRAM or the current ROM identity.
- Pass launcher, import/audit, input mapping, Time Crisis pedal/gun/crosshair,
  controller hot-plug, audio and game-switch smoke tests on physical Adreno and
  Mali devices.
- Pass 16 KB page-size validation. Android requires NDK apps to be rebuilt for
  those devices: [official guidance](https://developer.android.com/guide/practices/page-sizes).
- Compare logical-canvas screenshots with desktop and capture performance,
  thermals, audio underruns and frame pacing. The gate is no material
  regression from the same game's desktop emulation behaviour, not an
  unsupported promise that every phone runs every board at full speed.

## Workstream 2: Windows x86-64

### First supported build

- Target 64-bit Windows 10/11 using the MSYS2 UCRT64/MinGW toolchain first.
  This is the shortest route because the current project and dependencies are
  already GCC/pkg-config oriented. MSYS2 identifies UCRT64 as its recommended
  environment: [environment guide](https://www.msys2.org/docs/environments/).
- Keep desktop OpenGL 4.3/GLEW for gameplay and retain Vulkan/software
  presentation where the runtime supports it.
- Replace `zenity` with the platform picker implementation based on Windows
  `IFileOpenDialog`; use the shared service for ROM import and save export.
- Use the shared path service below local app data and the Windows Segoe UI
  system font.
- Port test temp/environment helpers and remove unconditional GNU options and
  `libm` links.

### CI and package

- Add a native Windows GitHub Actions job that configures UCRT64 with CMake +
  Ninja, runs all non-ROM tests, and performs a headless command-line smoke
  test.
- Package `WhittyArcade.exe`, required runtime DLLs, README,
  proprietary licence, third-party notices and a SHA-256 manifest as one
  `WhittyArcade-windows-x86_64.zip`.
- Verify the ZIP on clean Windows 10 and 11 machines with Intel, AMD and NVIDIA
  graphics where available. Test Xbox-style controllers, hot-plug, keyboard,
  mouse lightgun input, P1/P2 crosshairs, ROM import and paths containing
  spaces/non-ASCII text.
- Code signing and an installer are a later hardening milestone. The first
  release is a portable ZIP; it must not rely on an installed MSYS2 runtime.

After the UCRT64 package is stable, a separate MSVC/vcpkg build can provide a
more conventional Visual C++ toolchain. It is not on the critical path for the
first native `.exe`.

## Workstream 3: broader Linux distribution

### Portable package first

- Keep the current CachyOS/Arch-optimised tarball.
- Add a generic x86-64 build on a conservative glibc baseline and package it as
  `WhittyArcade-linux-x86_64.AppImage`.
- Bundle application libraries that cannot be assumed on target systems, but
  do not bundle the host's glibc or GPU driver stack. Check OpenGL 4.3/Vulkan
  capabilities at startup and report a useful error.
- Use the XDG desktop portal as the primary file/folder picker, retaining a
  documented fallback outside sandboxes. Test native Wayland and X11.
- Validate on Ubuntu, Debian, Fedora, openSUSE, Arch/CachyOS and SteamOS-class
  systems. AppImage's packaging model is documented in its
  [official introduction](https://docs.appimage.org/introduction/index.html).

### Flatpak second

- Add a reproducible Flatpak manifest using a maintained Freedesktop runtime.
- Use portals for ROM import/export and grant only the display, audio, GPU and
  controller access needed by the emulator.
- Publish through the project's own release channel first. Flathub submission
  and its current policy review are separate decisions.
- Exercise the same ROM-library and persistent-data migration tests inside the
  sandbox. Portal APIs are documented by
  [Flatpak](https://docs.flatpak.org/en/latest/portal-api-reference.html).

### Native distro packages last

- Produce `.deb` for the chosen Debian/Ubuntu baseline and `.rpm` for the
  chosen Fedora/openSUSE baselines only after AppImage and Flatpak are stable.
- Native packages use distro dependencies and require a test/rebuild matrix on
  every supported release; they are not merely renamed generic archives.
- Linux AArch64 is a later hardware/performance gate. Android ARM64 compiler
  portability helps, but desktop GPU capability and board performance still
  require separate validation.

## CI and public release matrix

| Platform | CI deliverable | Public asset |
|---|---|---|
| CachyOS x86-64 | Existing build, 46 tests, package smoke test | `.tar.gz` + checksum |
| Android ARM64 | NDK build/tests, APK install/smoke on emulator, physical-device release gate | AAB for Play; signed `.apk` + checksum only if direct public distribution is selected |
| Windows x86-64 | UCRT64 build/tests and clean-VM package smoke | `.zip` + checksum |
| Generic Linux x86-64 | baseline build/tests plus AppImage extraction/run smoke | `.AppImage` + checksum |
| Flatpak x86-64 | manifest build and sandbox smoke | bundle/repository metadata as selected |

Every release job must:

- build from the private tagged source commit;
- stage only binaries/assets/documentation, never source or ROM material;
- strip release binaries where appropriate and generate SHA-256 checksums;
- include the proprietary licence and third-party notices;
- publish to the public releases repository only after platform acceptance;
- extend the anonymous GitHub asset-download counter to recognise any
  directly published APK, ZIP, AppImage and selected Flatpak assets while
  excluding checksum downloads. Play-only installs use Play Console metrics.

## Planning estimate and sequence

These are engineering ranges for one developer after Preview 2, not release
date promises. Device/driver defects are the largest uncertainty.

| Milestone | Estimate | Depends on |
|---|---:|---|
| Shared platform foundation | 5-8 developer days | Preview 2 baseline |
| Android renderer feasibility spike | 3-5 developer days | Foundation renderer seam |
| Android usable ARM64 build, touch/storage/lifecycle and device QA | 20-35 developer days | Successful renderer spike |
| Windows UCRT64 build and portable ZIP | 5-8 developer days | Shared foundation |
| Generic Linux AppImage | 3-5 developer days | Shared foundation |
| Flatpak | 4-7 developer days | Portal picker and generic Linux validation |
| Each selected native `.deb`/`.rpm` family | 2-4 developer days plus ongoing maintenance | Stable portable Linux build |

Recommended issue order:

1. platform paths/assets and migration tests;
2. file-picker interface and Linux implementation;
3. single-surface/window ownership and GUI snapshot tests;
4. target-based CMake/dependency cleanup;
5. Android GLES shader/device spike;
6. Android Gradle shell, dependencies and ARM64 tests;
7. Android ROM picker, touch controls and lifecycle;
8. Android signed preview and physical-device gate;
9. Windows UCRT64 CI, picker/paths and clean-machine ZIP;
10. generic Linux AppImage, portal validation and distro matrix;
11. Flatpak, followed only then by requested native distro packages.

## Explicitly outside this scope

- ROM, firmware or key distribution;
- publishing private engine source in the public releases repository;
- Android 32-bit, Windows ARM64, Android TV, macOS or iOS in the first wave;
- a native Vulkan rasteriser before the GLES Android release gate;
- Play Store, Microsoft Store, Flathub or distro-repository approval;
- claiming pixel-perfect emulation or full speed on hardware that has not been
  measured.
