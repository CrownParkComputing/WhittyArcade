#!/usr/bin/env python3
"""Convert Burnout 3's Xbox boot media into portable Android-ready assets.

The XMV files contain video only. Their matching audio is stored as 34
independent ASF/WMA streams inside ovid/movie.xwb. This importer extracts the
bounded streams, transcodes each to AAC/M4A, and combines the known boot pairs
as H.264/AAC MP4 files. It requires only Python's standard library and an
ffmpeg executable at asset-build time; neither is a runtime plugin.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ASF_HEADER_GUID = bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")

# Confirmed by bank/video duration and stream order for the NTSC Burnout 3
# movie bank. The audio streams include a small amount of encoder padding;
# ffmpeg's -shortest trims that padding when producing the combined MP4.
BOOT_PAIRS = {
    "cri_rw30": 7,
    "Titles30": 11,
    "englis30": 33,
}


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_movie_bank(path: Path) -> tuple[bytes, list[dict[str, int]]]:
    data = path.read_bytes()
    if len(data) < 0x50 or data[:4] != b"WBND":
        raise ValueError(f"{path} is not an Xbox wave bank")
    version = u32(data, 4)
    if version != 3:
        raise ValueError(f"unsupported movie.xwb version {version}; expected 3")

    bank_offset, bank_length = struct.unpack_from("<II", data, 0x08)
    meta_offset, meta_length = struct.unpack_from("<II", data, 0x10)
    wave_offset, wave_length = struct.unpack_from("<II", data, 0x20)
    if bank_offset + bank_length > len(data) or wave_offset + wave_length > len(data):
        raise ValueError("wave bank segment extends past end of file")

    entry_count = u32(data, bank_offset + 4)
    entry_size = u32(data, bank_offset + 0x18)
    if entry_size != 24 or meta_length < entry_count * entry_size:
        raise ValueError("movie.xwb does not use the expected 24-byte entry layout")

    entries: list[dict[str, int]] = []
    for index in range(entry_count):
        offset = meta_offset + index * entry_size
        if offset + entry_size > len(data):
            raise ValueError(f"wave-bank entry {index} is truncated")
        flags_duration, mini_format, play_offset, play_length, loop_offset, loop_length = (
            struct.unpack_from("<IIIIII", data, offset)
        )
        start = wave_offset + play_offset
        end = start + play_length
        if start < wave_offset or end > wave_offset + wave_length or end > len(data):
            raise ValueError(f"wave-bank entry {index} has an invalid play region")
        if data[start : start + 16] != ASF_HEADER_GUID:
            raise ValueError(f"wave-bank entry {index} is not a standalone ASF stream")
        entries.append(
            {
                "index": index,
                "flags_duration": flags_duration,
                "mini_format": mini_format,
                "offset": start,
                "length": play_length,
                "loop_offset": loop_offset,
                "loop_length": loop_length,
            }
        )
    return data, entries


def run_ffmpeg(ffmpeg: str, arguments: list[str]) -> None:
    command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", *arguments]
    subprocess.run(command, check=True)


def transcode_audio(ffmpeg: str, source: Path, destination: Path) -> None:
    run_ffmpeg(
        ffmpeg,
        [
            "-i", str(source),
            "-map", "0:a:0",
            "-vn",
            "-c:a", "aac",
            "-b:a", "160k",
            "-movflags", "+faststart",
            str(destination),
        ],
    )


def combine_boot_movie(
    ffmpeg: str, video: Path, audio: Path, destination: Path, video_codec: str
) -> None:
    run_ffmpeg(
        ffmpeg,
        [
            "-i", str(video),
            "-i", str(audio),
            "-map", "0:v:0",
            "-map", "1:a:0",
            "-c:v", video_codec,
            "-preset", "medium",
            "-crf", "22",
            "-pix_fmt", "yuv420p",
            "-c:a", "copy",
            "-shortest",
            "-movflags", "+faststart",
            str(destination),
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--video-codec", default="libx264")
    parser.add_argument(
        "--extract-only",
        action="store_true",
        help="extract indexed ASF files without transcoding or combining boot movies",
    )
    args = parser.parse_args()

    ffmpeg = shutil.which(args.ffmpeg) if not Path(args.ffmpeg).is_file() else args.ffmpeg
    if not args.extract_only and not ffmpeg:
        parser.error(f"ffmpeg executable not found: {args.ffmpeg}")

    ovid = args.game_dir / "ovid"
    bank_path = ovid / "movie.xwb"
    data, entries = parse_movie_bank(bank_path)
    audio_dir = args.output / "audio"
    movie_dir = args.output / "movies"
    audio_dir.mkdir(parents=True, exist_ok=True)
    movie_dir.mkdir(parents=True, exist_ok=True)

    manifest_entries: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="burnout3-media-") as temp_name:
        temp = Path(temp_name)
        for entry in entries:
            index = int(entry["index"])
            start = int(entry["offset"])
            length = int(entry["length"])
            payload = data[start : start + length]
            asf_path = temp / f"movie_{index:02d}.wma"
            asf_path.write_bytes(payload)

            record: dict[str, object] = {
                **entry,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            if args.extract_only:
                output_audio = audio_dir / asf_path.name
                output_audio.write_bytes(payload)
                record["asset"] = str(output_audio.relative_to(args.output))
            else:
                output_audio = audio_dir / f"movie_{index:02d}.m4a"
                transcode_audio(str(ffmpeg), asf_path, output_audio)
                record["asset"] = str(output_audio.relative_to(args.output))
            manifest_entries.append(record)

        if not args.extract_only:
            for stem, audio_index in BOOT_PAIRS.items():
                source_video = ovid / f"{stem}.xmv"
                source_audio = audio_dir / f"movie_{audio_index:02d}.m4a"
                if not source_video.is_file():
                    raise FileNotFoundError(source_video)
                combine_boot_movie(
                    str(ffmpeg), source_video, source_audio,
                    movie_dir / f"{stem}.mp4", args.video_codec,
                )

    manifest = {
        "format": 1,
        "source": str(bank_path),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "audio_codec": "source/asf-wma" if args.extract_only else "aac-lc",
        "boot_pairs": BOOT_PAIRS,
        "entries": manifest_entries,
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Converted {len(entries)} movie-bank entries into {args.output}")
    if not args.extract_only:
        print(f"Created {len(BOOT_PAIRS)} Android-ready H.264/AAC boot movies")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"burnout3_media_import: {exc}", file=sys.stderr)
        raise SystemExit(1)
