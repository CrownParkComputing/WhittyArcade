#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <MANX.exe> <stage-directory> <debug-file>" >&2
    exit 2
fi

executable=$1
stage=$2
debug_file=$3

if [[ ! -f "$executable" ]]; then
    echo "Windows executable not found: $executable" >&2
    exit 1
fi
if [[ -e "$stage" || -e "$debug_file" ]]; then
    echo "Stage and debug destinations must not already exist." >&2
    exit 1
fi
if [[ -z "${MINGW_PREFIX:-}" || ! -d "$MINGW_PREFIX/bin" ]]; then
    echo "Run this script from an MSYS2 MinGW environment." >&2
    exit 1
fi

mkdir -p "$stage" "$(dirname "$debug_file")"
cp "$executable" "$stage/MANX.exe"
cp WINDOWS_README.md "$stage/README.md"
cp LICENSE "$stage/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$stage/THIRD_PARTY_NOTICES.md"

mapfile -t runtime_dlls < <(
    ldd "$executable" | awk -v prefix="$MINGW_PREFIX/bin/" '
        /=>/ { path=$3 }
        !/=>/ { path=$1 }
        index(path, prefix) == 1 && tolower(path) ~ /\.dll$/ { print path }
    ' | sort -u
)

if ldd "$executable" | grep -q 'not found'; then
    ldd "$executable" >&2
    echo "One or more runtime DLLs could not be resolved." >&2
    exit 1
fi
if [[ ${#runtime_dlls[@]} -eq 0 ]]; then
    echo "No UCRT64 runtime DLLs were discovered." >&2
    exit 1
fi

for dll in "${runtime_dlls[@]}"; do
    if [[ $(basename "$dll") == msys-2.0.dll ]]; then
        echo "The package must not depend on the MSYS2 POSIX runtime." >&2
        exit 1
    fi
    cp "$dll" "$stage/"
done

# Copy licences for each packaged runtime DLL and direct build dependency.
# Project-side static components are also covered by THIRD_PARTY_NOTICES.md.
mkdir -p "$stage/THIRD_PARTY_LICENSES"
mapfile -t license_packages < <(
    {
        for dll in "${runtime_dlls[@]}"; do
            pacman -Qqo "$dll" 2>/dev/null || true
        done
        printf '%s\n' \
            mingw-w64-ucrt-x86_64-sdl3 \
            mingw-w64-ucrt-x86_64-sdl3-ttf \
            mingw-w64-ucrt-x86_64-openal \
            mingw-w64-ucrt-x86_64-glew \
            mingw-w64-ucrt-x86_64-glm \
            mingw-w64-ucrt-x86_64-zlib \
            mingw-w64-ucrt-x86_64-minizip \
            mingw-w64-ucrt-x86_64-mpg123 \
            mingw-w64-ucrt-x86_64-vulkan-loader
    } | sort -u
)
for package in "${license_packages[@]}"; do
    if ! pacman -Qq "$package" > /dev/null 2>&1; then
        continue
    fi
    while IFS= read -r listed_path; do
        absolute_path=$listed_path
        if [[ $absolute_path != /* ]]; then
            absolute_path="/$absolute_path"
        fi
        case "$absolute_path" in
            "$MINGW_PREFIX"/share/licenses/*)
                if [[ -f "$absolute_path" ]]; then
                    relative_path=${absolute_path#"$MINGW_PREFIX/share/licenses/"}
                    mkdir -p "$stage/THIRD_PARTY_LICENSES/$(dirname "$relative_path")"
                    cp "$absolute_path" \
                       "$stage/THIRD_PARTY_LICENSES/$relative_path"
                fi
                ;;
        esac
    done < <(pacman -Qlq "$package")
done

objcopy --only-keep-debug "$stage/MANX.exe" "$debug_file"
strip --strip-unneeded "$stage/MANX.exe"
objcopy --add-gnu-debuglink="$debug_file" "$stage/MANX.exe"

if find "$stage" -maxdepth 1 -iname 'msys-2.0.dll' | grep -q .; then
    echo "Unexpected MSYS2 runtime in staged package." >&2
    exit 1
fi

(
    cd "$stage"
    find . -type f ! -name SHA256SUMS.txt -print0 |
        sort -z | xargs -0 sha256sum > SHA256SUMS.txt
)

echo "Staged $(find "$stage" -maxdepth 1 -type f | wc -l) files in $stage"
