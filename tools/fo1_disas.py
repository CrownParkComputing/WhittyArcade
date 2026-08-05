#!/usr/bin/env python3
"""
fo1_disas.py — FlatOut 1 x86 call-graph tracer.

Loads the XBE, maps sections into virtual address space, and uses Capstone
to recursively disassemble x86 code starting from the entry point. Follows
direct calls and unconditional jumps to build a static call graph.

Reports:
  - Function boundaries and sizes
  - Kernel thunk call sites (0x00273BC0 range)
  - Which ordinals each function calls
  - A tree view of the call graph from the entry point

Usage:
    python3 tools/fo1_disas.py flatout1_extracted/FlatOut.1.USA.XBOX-ZTM/default.xbe
"""

import struct
import sys
from collections import defaultdict
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# ── XBE loader ────────────────────────────────────────────────

def load_xbe(path):
    """Returns {va: bytearray} mapping for all sections."""
    with open(path, 'rb') as f:
        data = f.read()

    base = struct.unpack_from('<I', data, 0x104)[0]
    entry_raw = struct.unpack_from('<I', data, 0x10C)[0]
    # XBE entry point XOR key is at offset 0x118 (certificate key).
    xor_key = struct.unpack_from('<I', data, 0x118)[0]
    entry = entry_raw ^ xor_key
    nsec = struct.unpack_from('<I', data, 0x11C)[0]
    hdr_off = struct.unpack_from('<I', data, 0x120)[0] - base

    print(f"XBE: base=0x{base:08X} entry=0x{entry:08X} (raw=0x{entry_raw:08X} key=0x{xor_key:08X}) sections={nsec}")

    memory = {}
    sections = []
    for i in range(nsec):
        flags, va, vsize, raw, rsize, name_off = struct.unpack_from(
            '<IIIIII', data, hdr_off + i * 0x38)
        name = ''
        if name_off:
            end = data.find(b'\0', name_off)
            name = data[name_off:end].decode('ascii', errors='replace')
        if rsize > 0 and raw + rsize <= len(data):
            chunk = bytearray(data[raw:raw + rsize])
            if vsize > rsize:
                chunk.extend(b'\0' * (vsize - rsize))
            memory[va] = bytes(chunk)
            sections.append((va, vsize, name, flags))
        else:
            memory[va] = b'\0' * vsize
            sections.append((va, vsize, name, flags))

    print(f"Loaded {len(sections)} sections:")
    for va, vsize, name, flags in sections:
        executable = "X" if (flags & 0x00000004) else " "
        writable   = "W" if (flags & 0x00000002) else " "
        readable   = "R" if (flags & 0x00000001) else " "
        print(f"  0x{va:08X}  size={vsize:>8,}  [{readable}{writable}{executable}]  {name}")

    return memory, entry, sections


# ── Memory reader ─────────────────────────────────────────────

def read_byte(memory, va):
    """Read a single byte from virtual memory, or None."""
    for base, chunk in memory.items():
        end = base + len(chunk)
        if base <= va < end:
            return chunk[va - base]
    return None


def read_dword(memory, va):
    """Read a little-endian dword from virtual memory, or None."""
    b0 = read_byte(memory, va)
    b1 = read_byte(memory, va + 1)
    b2 = read_byte(memory, va + 2)
    b3 = read_byte(memory, va + 3)
    if b0 is None or b1 is None or b2 is None or b3 is None:
        return None
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)


def read_bytes(memory, va, count):
    """Read a slice from virtual memory, or truncated."""
    result = bytearray()
    for i in range(count):
        b = read_byte(memory, va + i)
        if b is None:
            break
        result.append(b)
    return bytes(result)


# ── Disassembler ──────────────────────────────────────────────

# Kernel thunk table range (FlatOut 1)
KERNEL_THUNK_BASE = 0x00273BC0
KERNEL_THUNK_END  = KERNEL_THUNK_BASE + 145 * 4

# Data export region
KDATA_BASE = 0x003EF000
KDATA_END  = 0x003F0000

# XBE sections from the known layout
TEXT_START = 0x00011000
TEXT_END   = 0x001A0000  # approximate
# FlatOut 1 has code in multiple RWX sections. The main code lives at:
CODE_REGIONS = [
    (TEXT_START, TEXT_END),         # .text
    (0x001B8CE0, 0x001CC420),       # D3D section
    (0x001CC420, 0x001EF540),       # more code
    (0x001EF540, 0x00202240),
    (0x00202240, 0x00229EE0),
    (0x00229EE0, 0x00232760),       # D3DX section
    (0x00232760, 0x002337C0),
    (0x002337C0, 0x00251A00),
    (0x00251A00, 0x0026AC80),
    (0x0026AC80, 0x00273BC0),       # main code block (36KB)
    (0x00273BC0, 0x002ADD80),       # kernel thunks + more code
    (0x002ADD80, 0x003C2160),       # large data/code section
]

def is_in_code(va):
    """Check if a VA falls within any code region."""
    for start, end in CODE_REGIONS:
        if start <= va < end:
            return True
    return False

# Kernel ordinal names (from flatout1_kernel_shim.c)
ORDINAL_NAMES = {
    15: "ExAllocatePool",
    16: "ExAllocatePoolWithTag",
    24: "ExQueryPoolBlockSize",
    165: "MmAllocateContiguousMemory",
    166: "MmAllocateContiguousMemoryEx",
    171: "ExFreePool",
    187: "NtClose",
    190: "NtCreateFile",
    197: "NtDuplicateObject",
    202: "NtOpenFile",
    210: "NtReadFile",
    218: "NtQueryVolumeInformationFile",
    277: "RtlEnterCriticalSection",
    289: "RtlInitAnsiString",
    291: "RtlInitializeCriticalSection",
    294: "RtlLeaveCriticalSection",
}


def is_kernel_thunk(va):
    return KERNEL_THUNK_BASE <= va < KERNEL_THUNK_END


def kernel_thunk_slot(va):
    """Returns the thunk table slot index for a given VA."""
    return (va - KERNEL_THUNK_BASE) // 4


def is_call_insn(mnemonic):
    return mnemonic in ('call', 'jmp')


def is_ret_insn(mnemonic):
    return mnemonic in ('ret', 'retf', 'iret', 'iretd')


def is_unconditional_jmp(mnemonic, op_str):
    return mnemonic == 'jmp' and not op_str.startswith(('j', 'n', 'l', 'g', 'e', 'a', 'b', 'c', 'o', 'p', 's'))


# ── Call graph tracer ─────────────────────────────────────────

def disassemble_function(memory, start_va, max_insns=2000):
    """
    Disassemble one function starting at start_va.
    Returns (insns, callees, terminal_type):
      insns: list of (va, mnemonic, op_str, size) tuples
      callees: list of direct call target VAs
      terminal_type: 'ret', 'jmp', 'thunk', 'branch', 'fallthrough', or 'unknown'
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    insns = []
    callees = []
    va = start_va
    visited_branches = set()

    for _ in range(max_insns):
        # Read up to 15 bytes (max x86 insn length)
        code = read_bytes(memory, va, 15)
        if len(code) == 0:
            return insns, callees, 'fallthrough'

        try:
            decoded = next(md.disasm(code, va))
        except StopIteration:
            return insns, callees, 'unknown'

        mnemonic = decoded.mnemonic
        op_str = decoded.op_str
        size = decoded.size
        insns.append((va, mnemonic, op_str, size))

        # Track direct call/jmp targets
        target = None
        if decoded.operands:
            op0 = decoded.operands[0]
            if op0.type == 1:  # IMM (immediate)
                target = op0.imm
            elif op0.type == 2:  # MEM (memory)
                if op0.mem.base == 0 and op0.mem.index == 0:
                    # Absolute address: [0xXXXXXXXX]
                    target = op0.mem.disp

        if target is not None:
            # Check if it's a kernel thunk call
            if is_kernel_thunk(target):
                callees.append(target)
            elif is_call_insn(mnemonic) or is_unconditional_jmp(mnemonic, op_str):
                # Follow direct calls and unconditional jumps
                if target not in visited_branches and is_in_code(target):
                    callees.append(target)

        # Termination conditions
        if is_ret_insn(mnemonic):
            return insns, callees, 'ret'
        if is_kernel_thunk(va):
            return insns, callees, 'thunk'
        if is_unconditional_jmp(mnemonic, op_str) and target:
            if not is_in_code(target):
                return insns, callees, 'jmp_outside'
            # Follow the jump
            visited_branches.add(target)
            va = target
            continue
        if mnemonic == 'hlt' or mnemonic == 'int3':
            return insns, callees, mnemonic

        va += size

    return insns, callees, 'max_insns'


def trace_call_graph(memory, entry_va, max_depth=8, max_functions=200):
    """
    Recursively trace the call graph from entry_va.
    Returns dict: {va: {'name': str, 'insns': list, 'callees': list, 'depth': int, 'kernel_calls': dict}}
    """
    functions = {}
    queue = [(entry_va, 0, 'entry')]
    visited = set()

    while queue and len(functions) < max_functions:
        va, depth, label = queue.pop(0)
        if va in visited:
            continue
        if not is_in_code(va):
            continue
        visited.add(va)

        insns, callees, term = disassemble_function(memory, va)
        if not insns:
            continue

        # Identify kernel calls
        kernel_calls = defaultdict(list)
        for callee in callees:
            if is_kernel_thunk(callee):
                slot = kernel_thunk_slot(callee)
                kernel_calls[slot].append(callee)

        # Try to read the thunk table to map slot→ordinal
        kernel_ordinals = {}
        for slot in kernel_calls:
            ordinal_va = KERNEL_THUNK_BASE + slot * 4
            entry_val = read_dword(memory, ordinal_va)
            if entry_val is not None:
                if entry_val & 0x80000000:
                    kernel_ordinals[slot] = entry_val & 0x7FFFFFFF
                elif KDATA_BASE <= entry_val < KDATA_END:
                    kernel_ordinals[slot] = f"KDATA@0x{entry_val:08X}"
                else:
                    kernel_ordinals[slot] = f"VA=0x{entry_val:08X}"

        name = label if label != 'entry' else f"sub_0x{va:08X}"
        functions[va] = {
            'name': name,
            'start': va,
            'end': insns[-1][0] + insns[-1][3] if insns else va,
            'insn_count': len(insns),
            'term': term,
            'callees': [c for c in callees if not is_kernel_thunk(c)],
            'kernel_calls': kernel_calls,
            'kernel_ordinals': kernel_ordinals,
            'depth': depth,
        }

        if depth < max_depth:
            for callee in functions[va]['callees']:
                if callee not in visited:
                    label = f"sub_0x{callee:08X}"
                    queue.append((callee, depth + 1, label))

    return functions


# ── Jump table reader ─────────────────────────────────────────

def read_jump_table(memory, va, count=64):
    """Try to read a jump table at va. Returns list of (index, pointer) tuples."""
    entries = []
    for i in range(count):
        ptr = read_dword(memory, va + i * 4)
        if ptr is None:
            break
        entries.append((i, ptr))
    return entries


# ── Report ────────────────────────────────────────────────────

def print_function(f, max_insns=6):
    """Print one function summary."""
    indent = "  " * f['depth']
    print(f"{indent}{f['name']}  0x{f['start']:08X}–0x{f['end']:08X}  "
          f"({f['insn_count']} insns, ends={f['term']})")

    # Show first few instructions
    insns = f.get('_insns', [])
    for va, mnem, ops, sz in insns[:max_insns]:
        print(f"{indent}    0x{va:08X}: {mnem:8s} {ops}")

    # Show kernel calls
    if f['kernel_calls']:
        for slot, vas in f['kernel_calls'].items():
            ordinal = f['kernel_ordinals'].get(slot, '?')
            name = ORDINAL_NAMES.get(ordinal, f"ordinal_{ordinal}") if isinstance(ordinal, int) else ordinal
            print(f"{indent}    KERNEL slot {slot}: {name}")


def print_call_tree(functions, entry_va, indent=0, visited=None, max_depth=5):
    """Print the call graph as a tree."""
    if visited is None:
        visited = set()
    if indent > max_depth * 2:
        return
    if entry_va not in functions:
        return
    f = functions[entry_va]

    prefix = "  " * indent
    kernel_str = ""
    if f['kernel_calls']:
        parts = []
        for slot in sorted(f['kernel_calls']):
            ordv = f['kernel_ordinals'].get(slot, '?')
            name = ORDINAL_NAMES.get(ordv, f"ord{ordv}") if isinstance(ordv, int) else str(ordv)
            parts.append(name)
        kernel_str = f"  [KERN: {', '.join(parts)}]"

    print(f"{prefix}├─ {f['name']} ({f['insn_count']} insns, →{f['term']}){kernel_str}")

    visited.add(entry_va)
    for callee in f['callees']:
        if callee not in visited and callee in functions:
            print_call_tree(functions, callee, indent + 1, visited, max_depth)


def print_unknown_calls(functions, entry_va, memory, visited=None):
    """Print indirect calls and jumps that we couldn't resolve."""
    if visited is None:
        visited = set()
    if entry_va not in functions or entry_va in visited:
        return
    visited.add(entry_va)
    f = functions[entry_va]

    # We don't have the raw insns in the functions dict yet, need to redisassemble
    insns, callees, _ = disassemble_function(memory, entry_va)
    for va, mnem, op_str, sz in insns:
        if mnem == 'call' and ('dword ptr' in op_str or 'word ptr' in op_str or '[' in op_str):
            if not any(c == entry_va for c in callees):  # already handled as direct
                print(f"  INDIRECT CALL: 0x{va:08X}: {mnem:8s} {op_str}")
        elif mnem == 'jmp' and ('dword ptr' in op_str or '[' in op_str):
            print(f"  INDIRECT JMP:  0x{va:08X}: {mnem:8s} {op_str}")

    for callee in f['callees']:
        print_unknown_calls(functions, callee, memory, visited)


# ═══════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════

def main():
    if len(sys.argv) < 2:
        xbe = "flatout1_extracted/FlatOut.1.USA.XBOX-ZTM/default.xbe"
    else:
        xbe = sys.argv[1]

    print(f"═══ FlatOut 1 XBE Disassembly Tracer ═══\n")
    memory, xbe_entry, sections = load_xbe(xbe)

    print(f"\n── Entry Point Analysis ──")
    print(f"XBE decoded entry: 0x{xbe_entry:08X}")

    # Read the entry point data (it's a jump/directory table)
    ep_data = read_bytes(memory, xbe_entry, 64)
    if ep_data:
        print(f"Bytes at entry point:")
        for i in range(0, min(64, len(ep_data)), 16):
            hexstr = ' '.join(f'{b:02x}' for b in ep_data[i:i+16])
            print(f"  0x{xbe_entry + i:08X}: {hexstr}")

    # Read jump table at entry point (pairs of code_ptr, data_id)
    jt = read_jump_table(memory, xbe_entry, 32)
    print(f"\nJump table at 0x{xbe_entry:08X}:")
    code_entries = []
    for idx, ptr in jt:
        in_code = is_in_code(ptr)
        marker = " ← CODE" if in_code else ""
        if ptr != 0:
            print(f"  [{idx:2d}] 0x{ptr:08X}{marker}")
        if in_code:
            code_entries.append(ptr)

    if not code_entries:
        print("ERROR: no code pointer found in jump table!")
        return 1

    # Probe each code entry to find the real CRT startup.
    # The first entry is often a HLT stub — skip trivial functions.
    print(f"\n── Probing code entries for CRT startup ──")
    best_entry = None
    best_score = 0
    all_functions = {}

    for code_va in code_entries[:20]:  # try first 20 code entries
        insns, callees, term = disassemble_function(memory, code_va, max_insns=5000)
        if not insns or len(insns) < 5:
            continue

        # Score: prefer functions with many insns, kernel calls, and sub-calls
        kernel_count = sum(1 for c in callees if is_kernel_thunk(c))
        sub_count = sum(1 for c in callees if not is_kernel_thunk(c))
        score = len(insns) + kernel_count * 20 + sub_count * 10

        print(f"  probe 0x{code_va:08X}: {len(insns):4d} insns, "
              f"{kernel_count} kernel calls, {sub_count} sub-calls, "
              f"→{term}  (score={score})")

        if score > best_score:
            best_score = score
            best_entry = code_va
            all_functions = trace_call_graph(memory, code_va, max_depth=6, max_functions=150)

    if best_entry is None:
        print("ERROR: no viable CRT startup found!")
        return 1

    code_entry = best_entry
    functions = all_functions
    print(f"\nSelected CRT startup: 0x{code_entry:08X} (score={best_score})\n")

    # Sort by depth then address
    sorted_funcs = sorted(functions.values(), key=lambda f: (f['depth'], f['start']))

    print(f"Found {len(functions)} functions:\n")
    for f in sorted_funcs:
        line = (f"{'  ' * f['depth']}{f['name']}  "
                f"0x{f['start']:08X}–0x{f['end']:08X}  "
                f"({f['insn_count']} insns, →{f['term']})")
        if f['kernel_calls']:
            parts = []
            for slot in sorted(f['kernel_calls']):
                ordv = f['kernel_ordinals'].get(slot, '?')
                name = ORDINAL_NAMES.get(ordv, f"ord{ordv}") if isinstance(ordv, int) else str(ordv)
                parts.append(name)
            line += f"  [KERN: {', '.join(parts)}]"
        print(line)

    print(f"\n── Call Tree ──\n")
    print_call_tree(functions, code_entry, max_depth=5)

    # Count kernel ordinals used
    all_ordinals = set()
    for f in functions.values():
        for slot, ordv in f['kernel_ordinals'].items():
            if isinstance(ordv, int):
                all_ordinals.add(ordv)
    if all_ordinals:
        print(f"\n── Kernel Ordinals Referenced ({len(all_ordinals)}) ──")
        for o in sorted(all_ordinals):
            name = ORDINAL_NAMES.get(o, '')
            print(f"  ordinal {o:3d}  {name}")

    # Identify the "main" function (largest function called by entry)
    entry_func = functions.get(code_entry)
    if entry_func:
        main_candidates = []
        for callee in entry_func['callees']:
            if callee in functions:
                main_candidates.append((functions[callee]['insn_count'], callee))
        if main_candidates:
            main_candidates.sort(reverse=True)
            print(f"\n── Likely Main/WinMain Candidates ──")
            for count, va in main_candidates[:3]:
                f = functions[va]
                print(f"  0x{va:08X}: {count} insns, depth={f['depth']}, "
                      f"calls {len(f['callees'])} sub-functions, "
                      f"ends with {f['term']}")

    print(f"\n── Done ──")
    return 0


if __name__ == '__main__':
    sys.exit(main())
