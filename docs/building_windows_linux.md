# Building MANX on Windows and Linux
MANX uses one CMake project for Windows and Linux. Android is not part
of these workflows. All current release jobs target x86-64 processors from
AMD or Intel.

## Windows 10/11

The supported Windows toolchain is native MSYS2 UCRT64. In an **MSYS2
UCRT64** terminal:

```bash
pacman -Syu --needed \
  git zip \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-sdl3-ttf \
  mingw-w64-ucrt-x86_64-openal \
  mingw-w64-ucrt-x86_64-glew \
  mingw-w64-ucrt-x86_64-glm \
  mingw-w64-ucrt-x86_64-zlib \
  mingw-w64-ucrt-x86_64-minizip \
  mingw-w64-ucrt-x86_64-mpg123 \
  mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-vulkan-headers

cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --parallel 2
ctest --test-dir build-windows --output-on-failure --parallel 2
```

The result is `build-windows/MANX.exe`. Running
`tools/stage_windows.sh` from the UCRT64 environment collects its transitive
UCRT64 DLL dependencies, licence files, checksums and a separate debug file.
The GitHub Actions workflow is the canonical example.

The packaged program is a native Windows GUI executable with a Windows 10/11
manifest, per-monitor V2 DPI awareness, a native `IFileOpenDialog`, Segoe UI
font discovery and `%LOCALAPPDATA%\MANX` storage. It does not depend
on `msys-2.0.dll`.

## Linux

Common configure, build and test commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

Install the development dependencies for the target distribution first. The
CI definitions in `.github/workflows/linux-portability.yml` are kept as the
machine-readable source of truth for Ubuntu, Debian, Fedora and openSUSE;
`.github/workflows/linux.yml` covers CachyOS/Arch.

The distribution artifacts are dynamically linked. Use the artifact built on
the same distribution family rather than copying a rolling-distribution
binary to an older glibc system.

## CI release inputs

Both desktop workflows run for pushes, pull requests and manual dispatches.
They build with two parallel jobs to keep the generated Moira tables within
the hosted runner memory limit, run the complete non-ROM test suite, execute
a headless `--list-roms` package smoke test, and publish checksummed artifacts.
No ROMs, firmware, keys, signing credentials or user data are put into a
build artifact.
