# MANX on Windows

MANX (formerly MANX) builds on Windows with MSVC + vcpkg + the
Vulkan SDK.

## Prerequisites

- Visual Studio 2022 (C++ Desktop workload)
- Vulkan SDK 1.3+
- vcpkg with the SDL3, OpenAL, libpng, libzip, zlib, vulkan, and
  wayland-protocols (the latter only if you also build a Wayland shim
  for cross-platform testing) ports

```cmd
vcpkg install sdl3 openal-soft libpng libzip zlib vulkan
```

## Building

```cmd
cmake -S . -B build-release ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DSDL3_VULKAN=ON
cmake --build build-release --config Release -j
```

The build produces `build-release/Release/MANX.exe`. The binary
name keeps the legacy `MANX` filename for Phase 1; the rename
lands in Phase 2.

## Burning the standalone

Burnout 3, Geometry Wars and the other Xbox 360 plugins ship as separate
executables that share the same launcher shim. They are produced as
`build-release/Release/Plugins/<plugin>.exe`.

## Notes

- The D3D12 path needs the Windows 10 1909+ SDK and a GPU driver that
  supports DXIL.
- For native multiscreen walls you need Windows 10 or later; the
  multi-monitor wall falls back to single-screen on Windows 7 / 8.
- ASan / ubsan builds use the `build-asan` and `build-sanitize` preset
  directories.
