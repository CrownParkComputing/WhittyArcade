# WhittyArcade for CachyOS / Arch Linux

This archive contains the x86-64 WhittyArcade executable. It does not contain
source code, game ROMs, firmware, keys, or other copyrighted game data.

Documentation and current downloads:
https://crownparkcomputing.github.io/WhittyArcade-Releases/

## Install dependencies

On an up-to-date CachyOS or Arch Linux system:

```bash
sudo pacman -S --needed \
  libglvnd mesa glu sdl2-compat sdl2_ttf openal glew \
  vulkan-icd-loader mpg123 zlib minizip freetype2 harfbuzz
```

Use the appropriate Vulkan/OpenGL driver for your GPU in place of `mesa` when
your system requires one.

## Verify and run

From the extracted release directory:

```bash
sha256sum -c WhittyArcade.sha256
chmod +x WhittyArcade
./WhittyArcade
```

The launcher opens automatically. Select **ROM Library / Import** to import
individual MAME-compatible ZIP archives or scan a folder. Archives are copied
unchanged into `~/.local/share/WhittyArcade/roms`; nothing is extracted or
repacked.

You must provide ROMs and firmware that you are legally entitled to use.
WhittyArcade does not provide them or link to downloads.

## Basic navigation

- Arrow keys / controller D-pad: move
- Enter / controller A: open
- Escape / controller B: back
- `5`: coin 1
- `1`: start 1
- `F2`: cabinet test switch
- `S`: emulator settings during gameplay
- `C`: controller mappings during gameplay
- Escape during gameplay: return to the launcher

Open **Controllers / Keyboard** in the launcher to inspect or change every
binding. Profiles can be set globally, per arcade board, or per game.

## Galaxian and UniWar S

Import the standard MAME `galaxian` or `uniwars` ZIP through **ROM Library /
Import**. No parent or firmware archive is required, and no ROM data is
included here.

- `5`: insert coin; `1`: start Player 1
- A/D: move left/right; Z: fire
- Cocktail Player 2: J/L to move, U to fire; `2` to start

All cabinet and player actions can be reassigned in the selected game's
controller profile.

## Time Crisis mouse gun

Use the World TS2 Ver.B `timecris` set with `namcoc71.zip` in the same ROM
folder. `namcoc74.zip` is not required for this game. No ROM or firmware is
included in this archive.

- Move the mouse to aim. Gameplay uses a cyan P1 target sight instead of the
  desktop pointer; two-player gun profiles have a separate red/magenta P2
  sight.
- Left click: fire
- Hold `Space`: press the pedal and stand/attack
- Release `Space`: hide and reload
- Right click: force cover/reload
- `5`: insert coin

The player is hidden by default, matching the cabinet pedal. Alternative gun,
pedal and trigger controls can be assigned in the game-specific controller
profile.

## Dirt Dash

Use the World DT2 Ver.C `dirtdash` set with `namcoc71.zip` in the same ROM
folder. The game carries its own M37710 program and does not need
`namcoc74.zip`. No ROM or firmware is included in this archive.

- Left/Right: steer; Up: accelerator; Down: brake
- Z/X: shift down/up
- V: view change; G: motion-stop cabinet switch
- `5`: insert coin; press the accelerator when prompted to start/select

The wheel, pedals, shifter and cabinet buttons can all be reassigned in the
game-specific controller profile.

## Aqua Jet

Use the World AJ2 Ver.B `aquajet` set with `namcoc71.zip` in the same ROM
folder. Like Dirt Dash it carries its own M37710 program and does not need
`namcoc74.zip`. No ROM or firmware is included in this archive.

- Left/Right: handlebar; Up: throttle lever
- P1 up/down: fore/aft body lean
- `5`: insert coin; `1`: start

The jet-ski cabinet has no brake, gearshift or view button, so only these
controls appear in the game-specific controller profile, where they can all
be reassigned.

## Licence

Official, unmodified WhittyArcade binaries are licensed for personal,
non-commercial use. Redistribution, commercial use, public exhibition,
hardware bundling, modification, and reverse engineering are not permitted
except where applicable law requires otherwise or a third-party component's
own licence grants rights in that component.

Read `LICENSE.txt` and `THIRD_PARTY_NOTICES.md` before use.

Copyright © 2026 Jonathan Whittingham / CrownParkComputing.
