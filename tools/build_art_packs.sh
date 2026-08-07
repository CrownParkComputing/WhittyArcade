#!/usr/bin/env bash
# Build one art pack per media folder, containing only the games MANX
# actually supports.
#
# A media library scraped for a whole MAME set is tens of gigabytes and
# almost none of it is a game this emulator runs. What a person downloading
# from the site wants is the artwork for the games they can play, in the
# folder layout MANX already reads - so a pack is unzipped straight into the
# media directory and works, with no renaming and no picking through it.
#
#   tools/build_art_packs.sh                        # everything
#   tools/build_art_packs.sh box2d marquee          # only those folders
#   MANX_MEDIA=/path/to/media tools/build_art_packs.sh
#
# The zips go to a GitHub release rather than into the website's git repo:
# they are a quarter of a gigabyte, the site is redeployed from that repo on
# every change, and artwork does not belong in a source history for ever.
# What lands in the site is a small index.json of names, sizes and checksums
# pointing at the release, so adding a folder here makes it appear on the
# page by itself.
#
# Pass --no-upload to build the zips and leave them on disk.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MEDIA="${MANX_MEDIA:-$HOME/.local/share/MANX/media}"
OUT="${MANX_ART_OUT:-$HOME/MANXOnline/public/downloads/art}"
STAGE="${MANX_ART_STAGE:-$HOME/.cache/manx-art-packs}"
RELEASE_REPO="${MANX_ART_REPO:-CrownParkComputing/MANX}"
RELEASE_TAG="${MANX_ART_TAG:-art-packs}"
BASE="https://github.com/$RELEASE_REPO/releases/download/$RELEASE_TAG"
UPLOAD=1
MANX_BIN="${MANX_BIN:-$HERE/build-clean/MANX}"

[ -d "$MEDIA" ] || { echo "No media library at $MEDIA" >&2; exit 1; }
[ -x "$MANX_BIN" ] || { echo "No MANX binary at $MANX_BIN" >&2; exit 1; }

mkdir -p "$OUT"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The supported sets, asked of the emulator rather than kept in a list here
# that would drift away from it. XDG_DATA_HOME is pointed somewhere empty so
# a developer's installed game plugins are not advertised as things the
# public build supports.
mkdir -p "$work/empty"
# The list is printed in full and then the process dies on the way out -
# heap corruption during shutdown, after every line has been written. The
# same crash is why the Windows tests cannot gate the build. Its exit status
# is deliberately not trusted here; the line count below is the real check.
XDG_DATA_HOME="$work/empty" MANX_HEADLESS=1 "$MANX_BIN" --list-roms 2>/dev/null |
  grep -oE '^  [a-z0-9_]+ - ' | tr -d ' -' | sort -u > "$work/supported" || true
supported_count=$(wc -l < "$work/supported")
[ "$supported_count" -gt 0 ] || { echo "No supported sets listed" >&2; exit 1; }
echo "$supported_count supported sets"

args=()
for argument in "$@"; do
  case "$argument" in
    --no-upload) UPLOAD=0 ;;
    *)           args+=("$argument") ;;
  esac
done
set -- "${args[@]+"${args[@]}"}"

mkdir -p "$STAGE"
folders=("$@")
if [ ${#folders[@]} -eq 0 ]; then
  mapfile -t folders < <(
    find "$MEDIA" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' |
    grep -vx metadata | sort)
fi

index="$work/index.json"
printf '{\n  "packs": [\n' > "$index"
first=1

for folder in "${folders[@]}"; do
  source_dir="$MEDIA/$folder"
  [ -d "$source_dir" ] || { echo "  $folder: not here, skipped"; continue; }

  stage="$work/stage/$folder"
  rm -rf "$work/stage"; mkdir -p "$stage"
  count=0
  while IFS= read -r name; do
    # Every extension the pack happens to use for this game. A library has
    # .png in one folder and .jpg in another and MANX reads either.
    for file in "$source_dir/$name".*; do
      [ -e "$file" ] || continue
      cp -n "$file" "$stage/" 2>/dev/null || true
      count=$((count + 1))
    done
  done < "$work/supported"

  if [ "$count" -eq 0 ]; then
    echo "  $folder: nothing for a supported game, skipped"
    continue
  fi

  zip_name="manx-art-$folder.zip"
  rm -f "$STAGE/$zip_name"
  # Zipped from one level up, so unzipping in the media directory puts the
  # files in the folder they belong in rather than loose.
  (cd "$work/stage" && zip -9 -q -r "$STAGE/$zip_name" "$folder")
  bytes=$(stat -c %s "$STAGE/$zip_name")
  sha=$(sha256sum "$STAGE/$zip_name" | cut -d' ' -f1)
  sha256sum "$STAGE/$zip_name" | sed "s| .*| $zip_name|" > "$STAGE/$zip_name.sha256"
  printf '  %-12s %4d file(s)  %6s\n' "$folder" "$count" \
         "$(numfmt --to=iec --suffix=B "$bytes")"

  [ $first -eq 1 ] || printf ',\n' >> "$index"
  first=0
  printf '    {"folder": "%s", "file": "%s/%s", "games": %d, "bytes": %d, "sha256": "%s"}' \
         "$folder" "$BASE" "$zip_name" "$count" "$bytes" "$sha" >> "$index"

  if [ "$UPLOAD" = 1 ]; then
    GH_TOKEN="${GH_TOKEN:-$(tr -d '\r\n\357\273\277' < "$HOME/cpcgithub.txt")}" \
      gh release upload "$RELEASE_TAG" \
        "$STAGE/$zip_name" "$STAGE/$zip_name.sha256" \
        --repo "$RELEASE_REPO" --clobber > /dev/null
    echo "               uploaded"
  fi
done

printf '\n  ],\n  "supportedGames": %d,\n  "built": "%s"\n}\n' \
       "$supported_count" "$(date -u +%Y-%m-%d)" >> "$index"
cp "$index" "$OUT/index.json"
echo "Wrote $OUT/index.json"
