#!/usr/bin/env python3
"""
fgui — Freebuff Game Utility Interface
=======================================
Generic Xbox game asset scanner. Point it at any extracted Xbox game
folder and it will:

  • Parse every .xbe (sections, kernel thunks, engine ID)
  • Classify every file by probing magic bytes (not extensions)
  • Probe inside BFS/XWB archives recursively
  • Auto-detect the game engine (RenderWare, Bugbear, …)
  • Report exactly which tools are needed for conversion
  • Output a conversion pipeline: XMV→MP4, XWB→MP3, DDS→PNG, etc.

All targets are universal formats (MP4/H.264+AAC, MP3, PNG) that work
on Linux and Android with zero Xbox plugin dependencies.

Usage:
    python3 tools/fgui.py game_data/xbox/burnout3
    python3 tools/fgui.py flatout1_extracted/FlatOut.1.USA.XBOX-ZTM

Output: plain-text report + <folder>/fgui_report.json.
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

# ══════════════════════════════════════════════════════════════
#  Magic-byte table
#  (offset, magic, mask, class, tool, note)
# ══════════════════════════════════════════════════════════════

MAGIC_TABLE = [
    # ── FMV — target: MP4/H.264+AAC ──────────────────────
    (0, b'XMV\x00',     None, 'fmv',    'ffmpeg',   'XMV (WMV9+WMA) → MP4/H.264+AAC'),
    (0, b'\x30\x26\xB2\x75', None, 'fmv', 'ffmpeg', 'ASF/WMV → MP4/H.264+AAC'),
    (0, b'\x00\x00\x01\xBA', None, 'fmv', 'ffmpeg', 'MPEG PS → MP4/H.264+AAC'),
    (0, b'\x00\x00\x01\xB3', None, 'fmv', 'ffmpeg', 'MPEG → MP4/H.264+AAC'),
    (-1, b'BINK',       None, 'fmv',    'bink-tools', 'Bink → MP4/H.264+AAC'),
    # RIFF must come AFTER WAVE so WAV files hit the audio match first.
    (0, b'RIFF',        None, 'fmv',    'ffmpeg',   'AVI → MP4/H.264+AAC'),

    # ── Audio — target: MP3 192kbps ──────────────────────
    (8, b'WAVE',        None, 'audio',  None,         'WAV/PCM — ready (or → MP3)'),
    (0, b'FSB4',        None, 'audio',  'fsb-extract', 'FMOD Sound Bank 4 → WAV → MP3'),
    (0, b'FSB5',        None, 'audio',  'fsb-extract', 'FMOD Sound Bank 5 → WAV → MP3'),
    (0, b'WBND',        None, 'audio',  'unxwb',     'XACT Wave Bank → WAV → MP3'),
    (-1, b'OggS',       None, 'audio',  None,         'OGG Vorbis — ready'),
    (0, b'\xFF\xFA',    b'\xFF\xFE', 'audio', 'ffmpeg', 'MP3 — ready'),
    (0, b'\xFF\xF2',    b'\xFF\xF7', 'audio', 'ffmpeg', 'MP3 — ready'),

    # ── GFX — target: PNG ────────────────────────────────
    (0, b'DDS ',        None, 'gfx',    'tex-convert', 'DDS → PNG (deswizzle NV2A tiling)'),
    (0, b'BMAP',        None, 'gfx',    'tex-convert', 'Bugbear bitmap → PNG'),
    (0, b'IMAG',        None, 'gfx',    'tex-convert', 'Bugbear texture → PNG'),
    (-1, b'DXT1',       None, 'gfx',    'tex-convert', 'DXT1 BC1 → PNG'),
    (-1, b'DXT3',       None, 'gfx',    'tex-convert', 'DXT3 BC2 → PNG'),
    (-1, b'DXT5',       None, 'gfx',    'tex-convert', 'DXT5 BC3 → PNG'),

    # ── k9 compression (Midway proprietary, LA Rush) ─────
    # k9CP = compressed payload, k9SF = signature/filelist
    (0, b'k9CP',        None, 'k9',      'k9-decompress', 'k9 compressed → unpack then classify'),
    (0, b'k9SF',        None, 'k9',      None,           'k9 signature/filelist'),
    (0, b'k9b\x00',     None, 'k9',      None,           'k9 binary blob'),

    # ── Archives ─────────────────────────────────────────
    (0, b'bfs1',        None, 'archive', 'bfs-unpack', 'Bugbear File System — unpack first'),
    (0, b'PK\x03\x04',  None, 'archive', 'unzip',     'ZIP archive'),
    (0, b'Rar!\x1A\x07', None, 'archive', 'unrar',    'RAR archive'),

    # ── RenderWare ───────────────────────────────────────
    (0, b'\x10\x00\x00\x00', b'\xFF\xFF\x00\x00', 'rw_stream', 'rw-decode',
     'RenderWare stream → extract audio to WAV → MP3'),
    (-1, b'RW\x00',     None, 'rw_stream', 'rw-decode', 'RenderWare binary stream'),

    # ── Xbox Packed Resource (LA Rush textures) ──────────
    (0, b'XPR0',        None, 'gfx',    'tex-convert', 'Xbox Packed Resource → deswizzle → PNG'),
    (0, b'XPR1',        None, 'gfx',    'tex-convert', 'Xbox Packed Resource v1 → PNG'),
    (0, b'XPR2',        None, 'gfx',    'tex-convert', 'Xbox Packed Resource v2 → PNG'),

    # ── Already portable ─────────────────────────────────
    (0, b'SQLi',        None, 'data',    None,       'SQLite — portable'),
    (0, b'\x89PNG',     None, 'gfx',     None,       'PNG — ready'),
    (0, b'<?xml',       None, 'data',    None,       'XML — portable'),
]

EXT_FALLBACK = {
    'txd': ('gfx',     'tex-convert', 'RenderWare TXD → extract to PNG'),
    'bgv': ('gfx',     'model-convert', 'RenderWare car model → GLTF/OBJ'),
    'bgd': ('gfx',     'model-convert', 'RenderWare car geometry → OBJ'),
    'bum': ('gfx',     'tex-convert', 'Bump/normal map → PNG'),
    'btv': ('gfx',     'model-convert', 'Car tint variation data'),
    'lwd': ('gfx',     'model-convert', 'Car LOD (low) → OBJ'),
    'hwd': ('gfx',     'model-convert', 'Car LOD (high) → OBJ'),
    'kfs': ('gfx',     'model-convert', 'Keyframe animation data'),
    'awd': ('audio',   'ffmpeg',      'Xbox ADPCM/WMA → WAV → MP3'),
    'wma': ('audio',   'ffmpeg',      'WMA → MP3'),
    'rws': ('rw_stream', 'rw-decode', 'RenderWare stream → extract then → MP3'),
    # Midway/k9 engine (LA Rush)
    'k9z': ('k9',      'k9-decompress', 'k9 compressed — needs decompressor'),
    'k9b': ('k9',      None,           'k9 binary blob'),
    'sig': ('k9',      None,           'k9 filelist signature'),
    'aclump': ('k9',   'k9-decompress', 'Asset clump container (k9)'),
    'lclump': ('k9',   'k9-decompress', 'Landscape clump container (k9)'),
    'ai':   ('k9',     None,           'AI behavior data (k9)'),
    'nav':  ('data',   None,           'Navigation mesh'),
    'seq':  ('data',   None,           'Sequence/script data'),
    'opp':  ('data',   None,           'Opponent/car data'),
    'car':  ('data',   None,           'Car definition'),
    'xpr':  ('gfx',    'tex-convert',  'Xbox Packed Resource → deswizzle → PNG'),
    'xsb':  ('audio',  'unxwb',        'XACT Sound Bank → extract → MP3'),
    'pak':  ('data',    None,           'Shader package (pixel/vertex shaders)'),
    # Data
    'dat': ('data',    None,           'Binary data — inspect manually'),
    'bin': ('data',    None,           'Binary blob — inspect manually'),
    'xml': ('data',    None,           'XML — portable'),
    'ico': ('data',    None,           'Icon — portable'),
    'xbe': ('xbe',     None,           'Xbox executable — recompile needed'),
}

# ══════════════════════════════════════════════════════════════
#  Conversion pipeline — asset class → target format + command
# ══════════════════════════════════════════════════════════════

PIPELINE = {
    'fmv': {
        'target': 'MP4/H.264+AAC',
        'note':   'Universal — hardware decode on Android, VLC/mpv on Linux',
        'cmds': [
            '# XMV/WMV/AVI → MP4 (single file):',
            'ffmpeg -i input.xmv -c:v libx264 -preset medium -crf 23 '
            '-c:a aac -b:a 128k -movflags +faststart output.mp4',
        ],
    },
    'audio': {
        'target': 'MP3 192kbps (or OGG Vorbis 160kbps)',
        'note':   'MP3 works on every platform with zero plugins. '
                  'OGG is smaller at same quality but needs MediaCodec on Android.  '
                  'NB: libmp3lame requires ffmpeg built with --enable-libmp3lame; '
                  'use built-in mp3 encoder (-c:a mp3) if unavailable.',
        'cmds': [
            '# FSB → WAV → MP3:',
            'fsb-extract input.fsb -o /tmp/wavs/',
            'for f in /tmp/wavs/*.wav; do '
            'ffmpeg -i "$f" -codec:a libmp3lame -b:a 192k "${f%.wav}.mp3"; done',
            '',
            '# XWB → WAV → MP3:',
            'unxwb input.xwb',
            'for f in *.wav; do '
            'ffmpeg -i "$f" -codec:a libmp3lame -b:a 192k "${f%.wav}.mp3"; done',
            '',
            '# Single AWD/WMA → MP3:',
            'ffmpeg -i input.awd -c:a mp3 -b:a 192k output.mp3',
            '',
            '# WAV → MP3 (already PCM, just compress):',
            'ffmpeg -i input.wav -c:a mp3 -b:a 192k output.mp3',
        ],
    },
    'gfx': {
        'target': 'PNG (or BC7 DDS for Vulkan GPU upload)',
        'note':   'PNG is universally loadable. For GPU textures, keep as BC7 DDS '
                  'after deswizzling NV2A tiling.',
        'cmds': [
            '# DDS → PNG (with NV2A deswizzle):',
            'convert_dds input.dds output.png   # uses manx_xbox_texture.c logic',
            '',
            '# Batch TXD extract → PNG:',
            'txd_extract Global.txd -o textures/',
        ],
    },
    'rw_stream': {
        'target': 'WAV (from RenderWare stream) → MP3',
        'note':   'RenderWare streams contain interleaved audio + events. '
                  'Extract audio tracks first, then encode to MP3.',
        'cmds': [
            '# RW stream → WAV:',
            'rw_decode input.rws -o output.wav',
            '# Then → MP3:',
            'ffmpeg -i output.wav -codec:a libmp3lame -b:a 192k output.mp3',
        ],
    },
    'archive': {
        'target': 'Extract to folder, then process contents',
        'note':   'Archive must be unpacked before individual files can be converted.  '
                  'BFS extraction needs a custom tool (the fgui BFS probe enumerates '
                  'entries but does not extract them — write a bfs_unpack helper or '
                  'extract by offset/size from the JSON report).',
        'cmds': [
            '# BFS unpack (needs custom tool — entries are in fgui_report.json):',
            '# Extract each file by seeking to entry.offset, reading entry.size bytes.',
            '# Then re-run fgui on the unpacked folder:',
            'python3 tools/fgui.py flatout_unpacked/',
        ],
    },
}


# ══════════════════════════════════════════════════════════════
#  XBE Header parser
# ══════════════════════════════════════════════════════════════

class XbeHeader:
    BASE_ADDR = 0x104; ENTRY_ADDR = 0x118; SECT_COUNT = 0x11C
    SECT_HDR = 0x120; THUNK_MIN_RUN = 40

    def __init__(self, data: bytes):
        if data[:4] != b'XBEH':
            raise ValueError('not an XBE')
        self.data = data
        self._cache: list[tuple] | None = None
        self._thunk_va: int | None = None

    def u32(self, off): return struct.unpack_from('<I', self.data, off)[0]

    @property
    def base(self):       return self.u32(self.BASE_ADDR)
    @property
    def entry(self):      return self.u32(self.ENTRY_ADDR)
    @property
    def section_count(self): return self.u32(self.SECT_COUNT)

    def _build(self):
        if self._cache is not None: return
        cache = []
        hdr = self.u32(self.SECT_HDR) - self.base
        for i in range(self.section_count):
            s = hdr + i * 0x38
            va, vsz, raw, rsz = self.u32(s+4), self.u32(s+8), self.u32(s+12), self.u32(s+16)
            flags = self.u32(s)
            name = self._sec_name(self.u32(s+20))
            cache.append((va, raw, rsz, vsz, flags, name))
        self._cache = cache

    def _sec_name(self, name_addr):
        if name_addr == 0: return '<unnamed>'
        off = name_addr - self.base
        if off < 0 or off >= len(self.data): return '<bogus>'
        end = self.data.find(b'\x00', off)
        if end < 0: end = min(off + 256, len(self.data))
        try: return self.data[off:end].decode('ascii', errors='replace')
        except: return '<binary>'

    def sections(self):
        self._build()
        yield from self._cache

    def va_to_raw(self, va):
        if self._cache:
            for sva, sraw, _, svsz, _, _ in self._cache:
                if sva <= va < sva + svsz:
                    return sraw + (va - sva)
        return va - self.base

    def detect_thunk_va(self):
        if self._thunk_va is not None: return self._thunk_va
        self._build()
        rdata = None
        for sva, sraw, srsz, svsz, _, name in self._cache:
            if name == '.rdata': rdata = (sva, svsz); break
        if not rdata:
            self._thunk_va = 0x0036B7C0; return self._thunk_va
        sva, svsz = rdata
        best_start, best_len = 0, 0
        cur_start, cur_len = 0, 0
        for va in range(sva, sva + svsz, 4):
            raw = self.va_to_raw(va)
            if 0 <= raw + 4 <= len(self.data):
                entry = self.u32(raw)
                # Kernel thunk = 0x80000000 | ordinal (0 < ordinal < 512).
                ordinal = entry & 0x7FFFFFFF
                if (entry & 0x80000000) and (0 < ordinal < 512):
                    if cur_len == 0: cur_start = va
                    cur_len += 1
                else:
                    if cur_len > best_len:
                        best_start, best_len = cur_start, cur_len
                    cur_len = 0
            else:
                if cur_len > best_len:
                    best_start, best_len = cur_start, cur_len
                cur_len = 0
        if cur_len > best_len: best_start, best_len = cur_start, cur_len
        self._thunk_va = best_start if best_len >= self.THUNK_MIN_RUN else 0x0036B7C0
        return self._thunk_va

    def thunk_ordinals(self):
        addr = self.detect_thunk_va()
        if addr == 0: return []
        raw = self.va_to_raw(addr)
        if raw < 0: return []
        ordinals = []
        for i in range(256):
            if raw + i*4 + 4 > len(self.data): break
            e = self.u32(raw + i*4)
            if e & 0x80000000: ordinals.append(e & 0x7FFFFFFF)
            elif ordinals: break
            else: ordinals.append(0)
        return ordinals


# ══════════════════════════════════════════════════════════════
#  Engine detection
# ══════════════════════════════════════════════════════════════

def detect_engine(sections: list[dict]) -> str:
    """Detect game engine from XBE sections.
    XMV/DSOUND/WMADEC/D3D/D3DX/etc. are Xbox XDK libraries, not engine
    markers.  Better signals: D3DX presence, localized string sections
    (Bugbear), and whether XONLINE comes before .text (Midway)."""
    names = {s['name'] for s in sections}
    has_d3dx = 'D3DX' in names
    has_online = 'XONLINE' in names
    # FlatOut 1 has localized string sections (sEnglish, sItalian, …);
    # LA Rush has D3DX + XONLINE but no localized sections.
    has_lang = any(s['name'].startswith('s') and len(s['name']) > 1
                   and s['name'][1:].istitle() for s in sections)
    if has_d3dx:
        if has_lang:
            return 'Bugbear (BFS)'
        if has_online:
            return 'Midway (k9 engine)'
        return 'Custom (D3DX)'
    return 'RenderWare'


# ══════════════════════════════════════════════════════════════
#  Magic-byte classification
# ══════════════════════════════════════════════════════════════

def classify_bytes(data: bytes):
    """Return (class, tool, note) or None."""
    if len(data) < 4: return None
    for offset, magic, mask, cls, tool, note in MAGIC_TABLE:
        if offset == -1:
            if magic in data[:128]:
                return (cls, tool, note)
        else:
            if offset + len(magic) > len(data):
                continue
            chunk = data[offset:offset + len(magic)]
            if mask is not None:
                chunk = bytes(a & b for a, b in zip(chunk, mask))
            if chunk == magic:
                return (cls, tool, note)
    return None


def classify_file(fpath: str, fsize: int) -> tuple:
    ext = os.path.basename(fpath).rsplit('.', 1)[-1].lower() if '.' in fpath else ''
    if fsize < 4:
        return ('unknown', None, 'Too small', ext)
    try:
        with open(fpath, 'rb') as f:
            hdr = f.read(128)
    except OSError:
        return ('unknown', None, 'Unreadable', ext)
    result = classify_bytes(hdr)
    if result:
        return result + (ext,)
    fb = EXT_FALLBACK.get(ext)
    if fb:
        return fb + (ext,)
    if ext == 'xbe':
        return ('xbe', None, 'Xbox executable', ext)
    return ('unknown', None, 'Unrecognised', ext)


# ══════════════════════════════════════════════════════════════
#  Archive probing
# ══════════════════════════════════════════════════════════════

def probe_bfs(path: str) -> list[dict] | None:
    try:
        fsize = os.path.getsize(path)
        if fsize < 0x18: return None
        with open(path, 'rb') as f:
            hdr = f.read(0x18)
        if hdr[:4] != b'bfs1': return None
        n = struct.unpack_from('<I', hdr, 0x10)[0]
        if n == 0 or n > 200000: return None
        if 0x18 + n * 0x20 > fsize: return None
        with open(path, 'rb') as f:
            f.seek(0x18)
            table = f.read(n * 0x20)
        entries = []
        for i in range(n):
            off = i * 0x20
            data_off = struct.unpack_from('<I', table, off + 4)[0]
            data_sz  = struct.unpack_from('<I', table, off + 8)[0]
            if data_sz < 1 or data_sz > fsize: continue
            if data_off < 0x18 or data_off + data_sz > fsize: continue
            with open(path, 'rb') as f:
                f.seek(data_off)
                probe = f.read(min(data_sz, 128))
            ct = classify_bytes(probe)
            cls, tool, note = ct if ct else ('data', None, 'binary')
            entries.append({'offset': data_off, 'size': data_sz,
                           'class': cls, 'tool': tool, 'note': note})
        return entries
    except (OSError, struct.error):
        return None


def probe_xwb(path: str) -> list[dict] | None:
    try:
        fsize = os.path.getsize(path)
        if fsize < 0x18: return None
        with open(path, 'rb') as f:
            hdr = f.read(0x18)
        if hdr[:4] != b'WBND': return None
        n = struct.unpack_from('<I', hdr, 0x0C)[0]
        if n == 0 or n > 10000: return None
        return [{'offset': 0, 'size': 0, 'class': 'audio',
                 'tool': 'unxwb', 'note': f'Wave {i} inside XWB'}
                for i in range(n)]
    except (OSError, struct.error):
        return None


ARCHIVE_PROBERS = {b'bfs1': probe_bfs, b'WBND': probe_xwb}


def probe_archive(path: str) -> list[dict] | None:
    if os.path.getsize(path) < 4: return None
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError:
        return None
    prober = ARCHIVE_PROBERS.get(magic)
    return prober(path) if prober else None


# ══════════════════════════════════════════════════════════════
#  Main scanner
# ══════════════════════════════════════════════════════════════

def scan_folder(game_dir: str) -> dict:
    report = {
        'game_dir':  os.path.abspath(game_dir),
        'game_name': os.path.basename(os.path.abspath(game_dir)),
        'game_engine': 'Unknown',
        'tools_required': defaultdict(set),
        'xbes': [],
        'files': defaultdict(lambda: defaultdict(list)),
        'file_counts': defaultdict(int),
        'file_sizes': defaultdict(int),
        'archives_found': {},
    }

    # ── Parse all XBEs ──────────────────────────────────────
    xbe_paths = []
    for root, _, files in os.walk(game_dir):
        for fn in files:
            if fn.lower().endswith('.xbe'):
                xbe_paths.append(os.path.join(root, fn))

    all_sections = []
    for xp in xbe_paths:
        with open(xp, 'rb') as f:
            data = f.read()
        try:
            xbe = XbeHeader(data)
        except ValueError:
            continue
        rel = os.path.relpath(xp, game_dir)
        sections = []
        for va, raw, rsize, vsize, flags, name in xbe.sections():
            sections.append({
                'name': name, 'va': f'0x{va:08X}',
                'raw_size': rsize, 'virtual_size': vsize,
                'flags': f'0x{flags:08X}',
            })
        all_sections.extend(sections)
        thunks = xbe.thunk_ordinals()
        report['xbes'].append({
            'path': rel, 'size': len(data),
            'base': f'0x{xbe.base:08X}', 'entry': f'0x{xbe.entry:08X}',
            'sections': sections,
            'thunks': {
                'va': f'0x{xbe.detect_thunk_va():08X}',
                'count': len(thunks),
                'active': len([o for o in thunks if o]),
            },
        })

    report['game_engine'] = detect_engine(all_sections)

    # ── Walk every file ─────────────────────────────────────
    tools: dict[str, set] = defaultdict(set)
    archive_queue = []

    for root, _, files in os.walk(game_dir):
        for fn in files:
            fpath = os.path.join(root, fn)
            rel = os.path.relpath(fpath, game_dir)
            fsize = os.path.getsize(fpath)
            cls, tool, note, ext = classify_file(fpath, fsize)

            report['files'][cls][note or 'ok'].append({
                'path': rel, 'size': fsize, 'ext': ext, 'note': note or '',
            })
            report['file_counts'][cls] += 1
            report['file_sizes'][cls] += fsize
            if tool:
                tools[tool].add(note or cls)

            if cls == 'archive':
                archive_queue.append((rel, fpath))

    # ── Probe archives ──────────────────────────────────────
    for rel, fpath in archive_queue:
        entries = probe_archive(fpath)
        if entries is None: continue
        report['archives_found'][rel] = {
            'size': os.path.getsize(fpath),
            'entry_count': len(entries),
            'contents': defaultdict(lambda: defaultdict(list)),
            'content_counts': defaultdict(int),
            'content_sizes': defaultdict(int),
        }
        arc = report['archives_found'][rel]
        for e in entries:
            cls, tool, note = e['class'], e['tool'], e['note']
            key = f"{rel}#0x{e['offset']:08X}"
            arc['contents'][cls][note or 'ok'].append({
                'path': key, 'size': e['size'],
                'class': cls, 'note': note or '',
            })
            arc['content_counts'][cls] += 1
            arc['content_sizes'][cls] += e['size']
            if tool:
                tools[tool].add(note or cls)

    report['tools_required'] = {k: sorted(v) for k, v in tools.items()}
    return report


# ══════════════════════════════════════════════════════════════
#  Report printing
# ══════════════════════════════════════════════════════════════

def _inv_table(counts, sizes, label=''):
    if not counts: return
    total_f = sum(counts.values())
    total_b = sum(sizes.values())
    if label:
        print(f'\n  ── {label} ──')
    print(f'  Files: {total_f}  |  Size: {total_b/(1024*1024):.1f} MB')
    print(f'  {"Class":<14s} {"Count":>6s}  {"Size":>10s}')
    print(f'  {"-"*14} {"-"*6}  {"-"*10}')
    for cls in ['fmv', 'gfx', 'audio', 'rw_stream', 'k9', 'archive', 'data', 'xbe', 'unknown']:
        c = counts.get(cls, 0); s = sizes.get(cls, 0)
        if c: print(f'  {cls:<14s} {c:>6d}  {s/(1024*1024):>8.1f} MB')


def _print_pipeline(r: dict):
    """Print the conversion pipeline section."""
    # Collect which asset classes are present
    classes_present = set()
    classes_present |= {k for k in r['file_counts'] if r['file_counts'][k] > 0}
    for arc in r.get('archives_found', {}).values():
        classes_present |= {k for k in arc['content_counts'] if arc['content_counts'][k] > 0}

    pipeline_classes = [c for c in ['fmv', 'audio', 'gfx', 'rw_stream', 'archive']
                       if c in classes_present]
    if not pipeline_classes:
        return

    print(f'\n  ── Conversion Pipeline (Linux + Android, no Xbox plugins) ──')

    for cls in pipeline_classes:
        pp = PIPELINE.get(cls)
        if not pp: continue
        c = r['file_counts'].get(cls, 0)
        for arc in r.get('archives_found', {}).values():
            c += arc['content_counts'].get(cls, 0)
        if c == 0: continue

        print(f'\n  ▸ {cls.upper()}  ({c} files)  →  {pp["target"]}')
        print(f'    {pp["note"]}')
        for cmd in pp['cmds']:
            print(f'    {cmd}')


def print_report(r: dict):
    sep = '═' * 66
    print(sep)
    print(f'  fgui — Xbox Game Scanner → Linux/Android Conversion')
    print(sep)
    print(f'  Game:   {r["game_name"]}')
    print(f'  Engine: {r["game_engine"]}')
    print(f'  Path:   {r["game_dir"]}')

    # ── XBEs ────────────────────────────────────────────────
    for x in r['xbes']:
        print(f'\n  ── XBE: {x["path"]} ──')
        print(f'  {x["size"]:,} bytes  base={x["base"]}  entry={x["entry"]}'
              f'  thunks={x["thunks"]["count"]} ({x["thunks"]["active"]} active)'
              f' at {x["thunks"]["va"]}')
        notable = {'.text','.rdata','.data','XMV','DSOUND','WMADEC','D3D','D3DX',
                   'XONLINE','XNET','XGRPH','XPP','DOLBY'}
        lang = []
        for s in x['sections']:
            if s['name'] in notable:
                print(f'    {s["name"]:<12s} {s["va"]:>10s}  {s["raw_size"]/1024:7.1f} KB')
            elif s['name'].startswith('s'):
                lang.append(s['name'][1:])
        if lang:
            print(f'    Languages: {", ".join(lang)} ({len(lang)} translations)')

    # ── File inventory ──────────────────────────────────────
    _inv_table(r['file_counts'], r['file_sizes'], 'All Files')

    # ── Samples ─────────────────────────────────────────────
    for cls, label in [('fmv','FMV'), ('audio','Audio'), ('gfx','GFX')]:
        d = r['files'].get(cls, {})
        if not d: continue
        print(f'\n  ── {label} Files ──')
        count = 0
        for note, flist in d.items():
            for f in flist[:6]:
                print(f'    {f["path"]:<50s}  {f["size"]/(1024*1024):.1f} MB')
                count += 1
                if count >= 8: break
            if count >= 8: break
        total = r['file_counts'].get(cls, 0)
        if total > 8:
            print(f'    ... and {total - 8} more')

    # ── Archives ────────────────────────────────────────────
    for arc_path, arc in r.get('archives_found', {}).items():
        print(f'\n  ── Archive: {arc_path} ──')
        print(f'  {arc["entry_count"]} entries, {arc["size"]/(1024*1024):.1f} MB')
        _inv_table(arc['content_counts'], arc['content_sizes'],
                   f'Contents of {arc_path}')

    # ── Tools needed ───────────────────────────────────────
    tools = r.get('tools_required', {})
    if tools:
        print(f'\n  ── Tools Required ──')
        for tool in sorted(tools):
            used_for = ', '.join(tools[tool][:6])
            if len(tools[tool]) > 6: used_for += f' (+{len(tools[tool])-6})'
            print(f'  {tool:<18s} for: {used_for}')

    # ── Conversion Pipeline ─────────────────────────────────
    _print_pipeline(r)

    print(f'\n{sep}')
    print(f'  JSON report → {os.path.join(r["game_dir"], "fgui_report.json")}')
    print(f'{sep}\n')


# ══════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════

def main():
    p = argparse.ArgumentParser(description='fgui — generic Xbox game scanner')
    p.add_argument('game_dir', nargs='?', default='game_data/xbox/burnout3')
    p.add_argument('--json-only', action='store_true')
    p.add_argument('--no-json', action='store_true')
    args = p.parse_args()

    gd = os.path.abspath(args.game_dir)
    if not os.path.isdir(gd):
        print(f'ERROR: {gd} is not a directory', file=sys.stderr)
        sys.exit(1)

    report = scan_folder(gd)

    if not args.json_only:
        print_report(report)
    else:
        print(json.dumps(report, indent=2, default=str))

    if not args.no_json:
        with open(os.path.join(gd, 'fgui_report.json'), 'w') as f:
            json.dump(report, f, indent=2, default=str)


if __name__ == '__main__':
    main()
