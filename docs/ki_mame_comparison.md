# MAME vs MANX — KI/Wolf Unit deep-dive findings

Sources (mamedev/mame): src/mame/rare/kinst.cpp, src/devices/cpu/mips/mips3.cpp,
mips3com.h, src/mame/shared/dcs.cpp, src/devices/machine/idectrl.cpp.
Ours: src/midway/mips_cpu.cpp, src/midway/wolfunit_session.cpp.

## Already fixed this session (verified vs MAME)
- endianness (LE), ADDI overflow, branch off-by-4, JAL link off-by-4,
  32-bit word shifts masking, MIPS-III 64-bit subset, exception vector
  0x80000180, interrupt condition incl. ERL, VBLANK=IP2, null-handler guard.

## CONFIRMED remaining discrepancies

### 1. Count/Compare timer (mips3.cpp 1113-1116, 1953, 2024-2034)  [HIGH corr.]
MAME:
- reset: Compare = 0xFFFFFFFF, Count = 0.
- Count register = (total_cycles - count_zero_time) / 2  -> **HALF cycle rate**.
- write Count: count_zero_time = total_cycles - val*2.
- write Compare: compare_armed = 1; CAUSE &= ~0x8000 (clear IP7); reschedule.
- IP7 set ONCE when Count reaches Compare (scheduled timer), gated by
  compare_armed -> **edge-triggered, one per Compare write**.
Ours:
- reset Compare = 0; Count full-rate (count += cycles) -> **2x too fast**.
- fire: `compare != 0 && count >= compare` -> sets IP7 EVERY step once passed
  (continuous, not edge). No armed flag.
Effect: our Count reads run 2x fast (any Count-based game timing is off); the
continuous fire is the storm the null-handler guard papers over.
FIX: half-rate Count read/write; compare_armed edge-trigger; reset 0xFFFFFFFF.

### 2. Missing MIPS-III instructions  [not hit yet, but WILL be at gameplay]
Runtime trap (KI_TRAP_OP) shows the game executes NONE of these *yet*, but
they are standard MIPS-III and the loaded program contains them (attract/
gameplay paths): LWU(0x27), DMULT/DMULTU/DDIV/DDIVU(special 0x1C-0x1F),
SDL/SDR(0x2C/0x2D), SYNC(special 0x0F=nop), LL/SC/LLD/SCD(0x30/0x38/0x34/0x3C).
FIX: add them (cheap; LL/SC as plain LW/SW+1, no multiprocessor).

### 3. COP0 registers  [low priority - TLB unused on kseg0/1]
We stub Index/BadVAddr/Count/Compare/Status/Cause/EPC/PRId/Config; ignore
EntryLo0/1, PageMask, Wired, EntryHi, Context, Random. TLB init writes them but
we run kseg0/1 unmapped, so harmless unless the game reads them back.

## STILL OPEN (why graphics don't load — not yet root-caused)
- Only 4 sectors read (dir + 128KB main program at LBA 4808), then the game
  runs its vblank-synced main loop but issues no further IDE reads.
- Candidates: (a) game still in long delay-loop init (interpreter slow, delays
  are 1M-10M spins); (b) waits on ATA IRQ (IP3) edge on read completion that we
  don't assert per-read; (c) DCS/other handshake for attract start.
- Next: trace whether the main loop ever calls the IDE read routine again, and
  whether it polls a state we don't satisfy.

## FBNeo cross-check (finalburnneo/FBNeo: d_kinst.cpp, ide.cpp, mips3 interp)
FBNeo has a clean non-DRC MIPS3 interpreter and a readable KI driver - a better
structural reference than MAME's DRC core. Confirmed:
- VBLANK = IRQ line 0 = IP2 (0x400); IDE = IRQ line 1 = IP3 (0x800). SEPARATE.
  Our per-frame IP2+IP3 assertion was wrong; VBLANK is IP2 only, held during
  the vblank slice; frame is DRAWN at vblank.
- IDE raises INTRQ (IP3) via the controller on completion, and ONCE PER SECTOR
  (ide.cpp update_transfer): after the guest reads all 512 bytes of a sector
  the next sector loads and INTRQ re-raises. Reading the STATUS register
  (ATA reg 7) clears INTRQ. Our model read the whole transfer at once with a
  single IRQ - an interrupt-driven loader stalls after sector 1.
- DCS is a full second CPU (ADSP-2100, Dcs2kRun), 2 IRQs/frame; sound status
  bit1 at port 0x90 = Dcs2kControlRead() & 0x800. Our always-set stub passes
  the handshake but plays no sound.
Applied: VBLANK->IP2 only; IDE raises IP3 on read/identify completion and
per-sector during the data transfer; STATUS read clears IP3.
OPEN: with the correct model the game now waits at its IP3 (IDE) poll
(0x8802DBC8) - the BSY/DRQ/IRQ *timing* of the read handshake still needs to
match FBNeo's (BSY during the command, DRQ+IRQ on completion). The previous
per-frame IP3 hack masked this. Next: model the ATA BSY phase and the exact
raise/clear ordering from ide.cpp.
