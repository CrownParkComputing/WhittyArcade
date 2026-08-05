# FlatOut 1 — Native recompilation progress

**Updated:** 2 August 2026  
**Primary milestone:** render the retail XBE via D3D8→Vulkan, loading all assets from `flatout.bfs`.

## Current status

The D3D8→Vulkan pipeline is proven (Clear→Present→readback produces correct pixels
on an RTX 3060), and the BFS asset archive is fully parseable. The game's x86 code
has **not** been recompiled yet — raw 32-bit Xbox x86 cannot execute in a 64-bit
Linux process. The next step is asset extraction and loading so textures/models
are ready when recompiled code arrives.

---

## Native Rendering Pipeline

### First Boot ✅
- 22/22 XBE sections loaded, 145/145 kernel thunks remapped at VA `0x00273BC0`
- All 9 B3 kernel stubs directly reusable; 4 new ordinals (186, 233, 235, 337) need stubs
- Build: `cmake --build build --target FlatOut1FirstBoot`

### D3D8→Vulkan Pipeline ✅
- `vulkan_d3d8.c` (from Burnout3Recomp) compiles independently for FlatOut 1
- GPU: NVIDIA RTX 3060 — `vulkan_d3d8_init(640, 480)` succeeds
- Clear(PresentTarget, amber) → Present() → readback produces correct RGBA8 frame
- **The D3D8→Vulkan backend is engine-agnostic** — works for FlatOut 1 too
- Build: `cmake --build build --target FlatOut1D3DProbe`

### D3D/D3DX Analysis
- D3D section (0x001B8CE0, 80 KB): compiled D3D8 code — needs binary disassembly
- D3DX section (0x00229EE0, 35 KB): compiled D3DX code — ordinal-based imports
- No D3DX function name strings in .text — all imports are ordinal-only
- Entry point (0x003C7F00): confirmed in data section, not executable

### Gap: No recompiled code
Pipeline is proven but FlatOut 1's native x86 hasn't been recompiled yet.
**Raw 32-bit Xbox x86 cannot execute directly in a 64-bit Linux process** — it
would crash instantly due to instruction set mismatches, calling convention
differences, and missing 32-bit thread context (FS/GS registers, TEB).

Static recompilation to C is strictly required before the game loop can run.

Entry point found: the XBE entry at 0x003C7F00 is a jump table — first pointer
at offset 0 is `0x00015088` (in .text). This is the CRT startup function to call
when recompiled code becomes available.

---

## BFS Asset Pipeline

### BFS Archive Format ✅
- `flatout.bfs`: 982 MB, 33,719 entries, "bfs1" magic
- Entry table: 0x14 bytes/entry (hash, data_off, size, csize, flags)
- Offsets are absolute (no base adjustment needed)
- zlib compression: `0x78` header, standard deflate
- Path strings embedded in entry data: `/data/cars/car_4/skin3.dds` etc.

### BFS VFS Library ✅
- `src/xbox/flatout1_bfs_vfs.c` / `.h` — full read-only VFS
- Peek cache: first 256 bytes of all 33,719 entries (~8.6 MB) read at open
- Path hashtable: built lazily on first `find_by_path` call, uses CRC32
- Zero seeks after open — classification, path extraction all from memory
- zlib decompression with 16 MB safety cap for corrupted size fields
- API: `open`, `close`, `find` (by CRC32 hash), `find_by_path`, `find_by_index`,
  `peek`, `iterate`, `entry_count`, `crc32`
- Build: `cmake --build build --target FlatOut1BFSTest`

### Classification Results
- First 5,000 entries: 78 Lua scripts, 1,678 zlib-compressed, 2,964 other
- Remaining 28,719: 8 raw DDS, 684 Lua, 4,303 zlib, 23,354 other
- Total: ~693 Lua, ~6,000 zlib entries across all 33,719
- Paths found: `data/cars/car_*/`, `data/tracks/*/textures/`, `data/scripts/`, etc.

### Path Lookup Verified
- `data/cars/car_4/skin3.dds` — 158 KB decompressed ✅
- `data/cars/car_2/dashboard.dds` — 202 KB decompressed ✅
- `data/tracks/town/textures/farmhouse_b.dds` — 31 KB decompressed ✅

### DDS Extraction
- `.dds` suffix matching works via peek cache path extraction
- Corrupted entries (size=0, giant csize) correctly skipped by safety cap
- Minor: path extraction from peek cache only catches ~30 entries; deeper
  entries need full decompression to reach embedded path strings

---

## Key Files

| File | Purpose |
|---|---|
| `src/xbox/flatout1_kernel_shim.c` | FlatOut 1 kernel layer (VA 0x00273BC0, 145 thunks) |
| `src/xbox/flatout1_first_boot.c` | XBE loader + kernel init + ordinal diagnostic |
| `src/xbox/flatout1_game_native.c` | D3D8/Vulkan pipeline probe + section analysis |
| `src/xbox/flatout1_bfs_vfs.c` | BFS archive reader with peek cache + zlib |
| `src/xbox/flatout1_bfs_vfs.h` | BFS VFS public API |
| `tools/flatout1_bfs_test.c` | diagnostic tool (classify, list, extract) |
| `tools/fgui.py` | generic XBE decomposer + asset inventory script |

## Build and Verification

```bash
# BFS archive diagnostic
cmake --build build --target FlatOut1BFSTest -j$(nproc)
./build/FlatOut1BFSTest flatout1_extracted/FlatOut.1.USA.XBOX-ZTM/flatout.bfs

# Boot visualiser — SDL3 window with FlatOut 1 amber boot screen
cmake --build build --target FlatOut1Boot -j$(nproc)
./build/FlatOut1Boot flatout1_extracted/FlatOut.1.USA.XBOX-ZTM

# D3D8→Vulkan pipeline probe
cmake --build build --target FlatOut1D3DProbe -j$(nproc)
./build/FlatOut1D3DProbe flatout1_extracted/FlatOut.1.USA.XBOX-ZTM

# Extract one file by path
./build/FlatOut1BFSTest flatout1_extracted/FlatOut.1.USA.XBOX-ZTM/flatout.bfs /tmp \
  data/cars/car_4/skin3.dds
```

## Boot Visualiser (2026-08-02)

### Status ✅
- `./build/FlatOut1Boot` loads a real 512×512 DXT5 DDS texture from BFS data
  entry 5456 and renders it in an SDL3 window with FlatOut 1 amber styling
- **Full pipeline proven:** BFS open → peek cache → find_by_index → zlib
  decompress → DDS parse → SDL texture upload → render loop
- Build: `cmake --build build --target FlatOut1Boot`
- Run: `./build/FlatOut1Boot flatout1_extracted/FlatOut.1.USA.XBOX-ZTM`

### Bugbear Manifest Indirection — CRACKED ✅
- Entries with path strings (0–29) are directory manifests, each record ~30 bytes
- Record format: f0(4=0) f1(4=entry_hash) f2(4=target_csize) f3(4=target_csize2)
  f4(4=0) path_len(2) path(N) + null padding
- **f2 = target data entry's csize** — the link from manifest to data
- Data entries (e.g. 5456–5877) have `hash=0x00000000` and actual DDS/TGA headers
- `find_by_path` now resolves this transparently: finds manifest entry, parses
  record from peek cache, matches csize to a data entry, returns data entry
- 8 DDS data entries found in first 10,000; resolution uses peek cache (no I/O)
- Boot visualizer now loads `data/cars/car_4/skin3.dds` via find_by_path → actual 512×512 DXT5 texture

### Files Created
| File | Purpose |
|---|---|
| `tools/flatout1_boot.c` | SDL3 boot visualiser: BFS→DDS→render |
| `src/xbox/flatout1_bfs_vfs.c` | BFS archive reader with peek cache + zlib |
| `src/xbox/flatout1_bfs_vfs.h` | BFS VFS public API |
| `tools/flatout1_bfs_test.c` | BFS diagnostic tool (classify, list, extract) |

### DXT5 DDS Decoder ✅
- `tools/dxt_decode.h` — standalone DXT1/DXT3/DXT5 software decoder to RGBA8888
- No GPU, no Vulkan, no allocations — pure C, self-contained in one header
- Boot visualiser now decodes compressed textures correctly (not noise)
- Verified: `data/cars/car_4/skin3.dds` (512×512 DXT3) renders via path lookup
- Build: `cmake --build build --target FlatOut1Boot`

## Recompilation Analysis (2026-08-02)

### XBE Entry Point — Init Table, Not Code ✅
- Decoded entry: `0x003D7E84` (in `.data` section `[   ]`, not executable)
- Structure: pairs of `(function_ptr, type_id)` — 15+ entries
- Function pointers point to `0x0026xxxx–0x0027xxxx` (36KB RWX section)
- **The Xbox kernel walks this table at boot** — our kernel shim does not
- Type IDs: `0x00070001`, `0x0001007E`, `0x00010034` etc. (priority/type encoding)
- Tool: `python3 tools/fo1_disas.py <xbe>` — Capstone-based call-graph tracer

### Init Functions Probed ✅
- 15 init functions in `0x0026AE00–0x0027283F` range, all small (7–145 insns)
- **None call kernel thunks directly** — they call into `.text` code (0x00011000)
- Top candidate: `0x002725C1` (145 insns, →ret) — scored highest
- All terminate with `ret` or `hlt` (no infinite loops — they're one-shot init callbacks)

### .text Section — Real C++ Game Code ✅
- First function at `0x00011000`: `__thiscall` C++ method (uses `ecx` as `this`)
- SSE instructions present (`movss`, `comiss`, `subss`, `prefetchnta`)
- `stdcall` convention (`ret 4`, `ret 0xc`)
- Calls sub-functions within `.text` (e.g. `call 0x1a170`)
- **No reference to entry table (0x003D7E84) found in .text**
  → The Xbox kernel itself walks the init table, not XBE code

### Kernel Thunk Table Confirmed ✅
- `0x00273BC0`: 145 entries, ordinals 1–360
- First 20 slots match expected ordinals: RtlEnterCriticalSection, MmAllocateContiguousMemory, ExAllocatePoolWithTag, RtlInitAnsiString, etc.
- Kernel data exports at `0x003EF000` (tick count, kernel version, hardware info)

### Recompilation Strategy

**Approach: Write a native CRT entry point that walks the init table.**

The Xbox kernel normally reads the entry table at `0x003D7E84` and calls each
`(func, type_id)` pair in order. We need a native replacement:

1. Parse the entry table: read pairs until a null entry ✅
2. For each pair, set up the emulated stack and call the function via the
   kernel dispatch mechanism (similar to `kernel_thunk_dispatch`)
3. The called function runs in the emulated register context, may call
   kernel thunks, and returns

Each init function at `0x0026xxxx` needs to be **recompiled** or **emulated**:
- Option A: static recompilation to C (like Burnout 3's `sub_00156400`)
- Option B: x86 instruction emulation via a VM (simpler but slower)
- **Recommended: hybrid** — recompile the init table walker natively,
  then use emulation/SIGSEGV recovery for the bulk of game code

The `.text` code (0x00011000–0x001B8CE0) is the actual game engine and needs
full recompilation eventually.

### Native CRT Entry Point ✅ (2026-08-02)
- `flatout1_crt_walk_init_table()` — two-pass parser: collects all 16 entries,
  scans each for indirect kernel thunk calls (`ff 15` pattern), estimates
  function sizes via `ret`/`int3`/`hlt` detection
- `flatout1_crt_dispatch()` — iterates entries, sets up emulated register
  context (eax/ecx/edx/ebx/esi/edi/ebp=0, fake return on stack), calls
  `native_stub()` for each recompiled entry, reports dispatched/skipped
- New ordinals discovered: 81, 87 (added to kernel_arg_bytes with 0-byte stubs)
- Ordinals 83, 85, 129, 149, 159, 161, 301 already existed — confirmed
- Tool: `cmake --build build --target FlatOut1CRTTest`
- Run: `./build/FlatOut1CRTTest flatout1_extracted/FlatOut.1.USA.XBOX-ZTM`

**Init table findings:**
| # | Address | Size | Type ID | Kernel thunks (slot→ordinal) |
|---|---------|------|---------|------------------------------|
| 0 | 0x0026AE00 | 25 | BOOT_INIT | *(zero-filled stub)* |
| 1 | 0x0026AE0C | 13 | INIT_DATA | *(zero-filled stub)* |
| 2 | 0x0026AE88 | 148 | INIT_VTABLE | *(zero-filled stub)* |
| 3 | 0x0026BE48 | 60 | INIT_RENDER | slot64→129, slot63→161, slot70→149 |
| 4 | 0x0026BE84 | 39 | INIT_AUDIO | slot64→129, slot63→161, slot70→149 |
| 5 | 0x0026BE89 | 34 | INIT_INPUT | slot64→129, slot63→161, slot70→149 |
| 6 | 0x0026BEAB | 109 | INIT_SYSTEM | slot64→129, slot63→161, slot70→149 |
| 7 | 0x00271B21 | 28 | INIT_SUBSYSTEM | slot64→129, slot63→161 |
| 8 | 0x00271D58 | 48 | INIT_RESOURCE | slot64→129, slot63→161, slot73→159, slot84→17 |
| 9 | 0x00271F84 | 15 | INIT_PHYSICS | slot2→277, slot134→85, slot1→294, slot8→301 |
| 10 | 0x0027255F | 55 | INIT_SCRIPT | slot63→161 |
| 11 | 0x002725B5 | 12 | INIT_THREAD | slot63→161, slot64→129 |
| 12 | 0x002725C1 | 106 | INIT_GAME | slot63→161, slot64→129 |
| 13 | 0x00272799 | 94 | INIT_NETWORK | slot64→129, slot63→161, slot139→87, slot138→83, slot140→81 |
| 14 | 0x0027280C | 51 | INIT_UI | slot64→129, slot63→161, slot139→87, slot138→83, slot140→81 |
| 15 | 0x0027283F | 76 | (type=0) | slot64→129, slot63→161, slot139→87, slot138→83, slot140→81 |

**13 of 16 entries call kernel thunks.** The dispatch framework is ready —
plug in recompiled C stubs via `native_stub` pointer per entry.

## First Recompiled Stub: fo1_init_06 (INIT_SYSTEM) ✅ (2026-08-02)

Entry 6 at `0x0026BEAB` (109 bytes) has been hand-translated from x86 to C
and plugged into the CRT dispatch:

- **`fo1_init_06_stub()`** in `src/xbox/flatout1_kernel_shim.c` — faithful
  stdcall translation: reads `(state*, out1*, out2*)` via `STACK_ARG()`,
  does the flag-state math (`o1 = ~f8 & f0 | common`, `o2 = ~f0 & f8 | common`,
  `common = f4 & f8 & f0`), calls kernel slots 64 (ord 129) and 63 (ord 161)
  through `fo1_call_kernel_slot()`, clears `state->f4`, sets `state->f8 = f0`,
  returns `(*out1 | *out2) ? 1 : 0` in `g_eax`.
- **`fo1_call_kernel_slot(slot)`** — resolves the thunk-table synthetic VA,
  dispatches through `recomp_lookup_kernel()` (zeroes eax/ecx/edx first so
  value-only stubs behave deterministically).
- **Per-entry args** — `fo1_init_entry` gained `args[4]`/`arg_count`;
  `flatout1_crt_set_init_args()` stores them, dispatch pushes them
  right-to-left (stdcall) with a fake return address so `STACK_ARG(n)`
  reads `args[n]`.
- **CRT stack** — dispatch now provides an emulated stack
  (`XBOX_STACK_BASE 0x03FE0000`, limit `0x03F00000` above the heap) instead
  of trusting a stale/zero `g_esp`, which previously wrapped to `0xFFFFFFFC`
  and faulted on the first push.  `g_eax` is no longer clobbered after the
  stub so return values survive.

**Verified end-to-end** (`./build/FlatOut1CRTTest`): all six assertions pass —
`out1=0xFF000000`, `out2=0x00FF0000`, `state->f4→0`, `state->f8→0xFF000000`,
`eax→1`, and the idle path (`f4==0`) zeroes both outputs and returns 0.

## Next Milestones

1. ~~Implement general manifest→data resolution in `find_by_path` (csize lookup)~~ ✅
2. ~~Wire BFS VFS into kernel shim (NtOpenFile/NtReadFile/NtClose)~~ ✅
3. ~~Parse DXT5 compressed DDS properly~~ ✅
4. ~~Write native CRT entry that walks the init table and calls each function~~ ✅
5. ~~Recompile first init function (fo1_init_06) and dispatch it~~ ✅
6. Recompile the remaining 12 thunk-calling init functions (INIT_RENDER,
   INIT_AUDIO, INIT_INPUT, INIT_SUBSYSTEM, INIT_RESOURCE, INIT_PHYSICS,
   INIT_SCRIPT, INIT_THREAD, INIT_GAME, INIT_NETWORK, INIT_UI, +final)
   and give ordinals 129/161/149/87/83/81 real semantics (semaphore/event/
   critical-section based on disassembly of each caller)
7. Load real FlatOut 1 textures through the Vulkan D3D8 backend
8. Full static recompilation of .text game code
