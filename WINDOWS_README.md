# WhittyArcade for Windows 10/11

This archive contains the native 64-bit Windows build of WhittyArcade and its
required runtime DLLs. It contains no source code, game ROMs, firmware, keys,
or other copyrighted game data.

## Run

Extract the entire ZIP, keep the DLLs beside `WhittyArcade.exe`, and double
click `WhittyArcade.exe`. Windows 10 or 11 x86-64 and an OpenGL 4.3-capable
graphics driver are required. Vulkan is optional at runtime unless selected in
the renderer settings.

The package is portable; no installer or administrator access is required.
Settings, controller mappings, high scores and saves are stored below
`%LOCALAPPDATA%\WhittyArcade`, so upgrading the extracted application
directory does not remove them.

The first-run wizard uses the standard Windows folder picker. Select the
folders where your MAME-compatible ZIP archives and CHD images already live,
or accept the recommended `%LOCALAPPDATA%\WhittyArcade` folders. Files are
read in place; nothing is imported, copied, extracted or repacked. Change the
paths later under **Settings > ROM and CHD folder locations**.

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

## Verify

`SHA256SUMS.txt` contains hashes for every packaged file. From PowerShell:

```powershell
Get-FileHash .\WhittyArcade.exe -Algorithm SHA256
```

Compare the result with the `WhittyArcade.exe` line in `SHA256SUMS.txt`.

## Licence

Official, unmodified WhittyArcade binaries are licensed for personal,
non-commercial use. Redistribution, commercial use, public exhibition,
hardware bundling, modification, and reverse engineering are not permitted
except where applicable law requires otherwise or a third-party component's
own licence grants rights in that component.

Read `LICENSE.txt`, `THIRD_PARTY_NOTICES.md`, and the files in
`THIRD_PARTY_LICENSES` before use.

Copyright © 2026 Jonathan Whittingham / CrownParkComputing.
