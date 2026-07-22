# WhittyArcade for Linux x86-64

This directory contains a WhittyArcade executable built and tested on the
distribution named by the download artifact. It contains no game ROMs,
firmware, keys, or other copyrighted game data.

Use the artifact matching your distribution family. The executable is
dynamically linked and expects the normal SDL2, SDL2_ttf, OpenAL, OpenGL,
GLEW, Vulkan loader, zlib, MiniZip, mpg123, and GLM packages supplied by that
distribution. The project currently publishes CI artifacts for Ubuntu 24.04,
Debian 13, Fedora 43, openSUSE Tumbleweed, and CachyOS/Arch Linux.

From the extracted directory:

```bash
sha256sum -c WhittyArcade.sha256
chmod +x WhittyArcade
./WhittyArcade
```

The launcher can import individual MAME-compatible ZIP archives or scan a ROM
folder. Imported archives are copied unchanged below
`$XDG_DATA_HOME/WhittyArcade/roms` (normally
`~/.local/share/WhittyArcade/roms`). Settings and saves are stored below
`$XDG_CONFIG_HOME/WhittyArcade` (normally `~/.config/WhittyArcade`).

You must provide ROMs and firmware that you are legally entitled to use.
WhittyArcade does not provide them or link to downloads.

Official, unmodified WhittyArcade binaries are licensed for personal,
non-commercial use. Read `LICENSE.txt` and `THIRD_PARTY_NOTICES.md` before
use.

Copyright © 2026 Jonathan Whittingham / CrownParkComputing.
