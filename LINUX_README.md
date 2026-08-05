# MANX on Linux

MANX (formerly MANX) builds on Linux with CMake, GCC or Clang,
SDL3, OpenGL 4.3, Vulkan, OpenAL, and the bundled third-party cores
(Musashi, moira). All Linux builds are GPL-3.

## Distro packages

### Arch / Manjaro / CachyOS

```bash
sudo pacman -S --needed \
  cmake ninja gcc vulkan-icd-loader libvulkan.so vulkan-tools \
  sdl3 openal libpng libzip zlib sdl3_image sdl3_ttf sdl3_mixer \
  wayland wayland-protocols libxkbcommon
```

CMake flags for native Wayland:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DSDL3_VULKAN=ON
cmake --build build-release -j
```

### Debian / Ubuntu

```bash
sudo apt install \
  build-essential cmake ninja-build pkg-config \
  libsdl3-dev libvulkan-dev vulkan-tools libopenal-dev \
  libpng-dev libzip-dev zlib1g-dev libwayland-dev libxkbcommon-dev
```

### Fedora

```bash
sudo dnf install \
  cmake ninja-build gcc-c++ \
  SDL3-devel vulkan-devel vulkan-tools openal-devel \
  libpng-devel libzip-devel zlib-devel wayland-devel libxkbcommon-devel
```

## Running

```bash
./build-release/manx -rompath ./roms
```

(The binary is currently still named `MANX` until the Phase 2
identifier rename; the manx symlink is staged for that release.)

## Building a flatpak

A flatpak manifest is on the roadmap; not yet provided.

## Notes

- For 3-screen System 246 walls, you need a primary display that
  reports >= 5760 pixels of horizontal resolution. Anything below that
  will refuse the wall layout at startup.
- For Vulkan validation in development, install the LunarG SDK and run
  with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.
- The runtime prefers Wayland; X11 falls back automatically but loses
  native multiscreen wall support.
