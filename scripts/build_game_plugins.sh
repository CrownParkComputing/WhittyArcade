#!/usr/bin/env bash
# Builds the native game plugins and installs them where the arcade looks.
#
# These are deliberately NOT part of the arcade's CMake build: the whole point
# of the plugin path is that a game ships and updates without rebuilding the
# host. But that cuts both ways - nothing rebuilds them when the ABI changes
# either, and the loader then refuses every installed copy with "built for
# plugin ABI 1, this arcade speaks 2". This script is the answer to that, and
# it prints the ABI it built against so the mismatch is obvious.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${MANX_GAME_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/MANX/games}"
out="${root}/build/plugins"

abi="$(sed -n 's/^#define MANX_GAME_ABI_VERSION \([0-9]*\)u\?$/\1/p' \
       "${root}/include/manx_game_plugin.h")"
echo "Building game plugins against ABI ${abi}"

mkdir -p "${out}"

build_edition() {
    local short_name="$1" edition="$2"
    local dir="${out}/${short_name}"
    mkdir -p "${dir}"
    # -O2 and no -ffast-math: the simulation must stay bit-identical across
    # machines for lockstep, and fast-math licenses the compiler to reassociate
    # float arithmetic differently depending on what it feels like inlining.
    c++ -std=c++20 -O2 -fPIC -shared -fvisibility=hidden \
        -DGW_EDITION="${edition}" \
        -I"${root}/include" \
        "${root}/src/geometrywars/gw_plugin.cpp" \
        "${root}/src/geometrywars/gw_game.cpp" \
        -o "${dir}/${short_name}.so"
    echo "  built ${short_name}.so"
}

build_edition geometrywars 1
build_edition geometrywars2 2

# Bubble Bobble is one game rather than a pair of editions, so it gets its own
# build rather than an edition define nothing reads.
build_bubblebobble() {
    local dir="${out}/bubblebobble_native"
    mkdir -p "${dir}"
    c++ -std=c++20 -O2 -fPIC -shared -fvisibility=hidden \
        -I"${root}/include" -I"${root}/third_party/stb" \
        "${root}/src/bubblebobble/bb_plugin.cpp" \
        "${root}/src/bubblebobble/bb_game.cpp" \
        -o "${dir}/bubblebobble_native.so"
    echo "  built bubblebobble_native.so"
}
build_bubblebobble

build_jetpac() {
    local dir="${out}/jetpac_native"
    mkdir -p "${dir}"
    c++ -std=c++20 -O2 -fPIC -shared -fvisibility=hidden \
        -I"${root}/include" \
        "${root}/src/jetpac/jp_plugin.cpp" \
        "${root}/src/jetpac/jp_game.cpp" \
        -o "${dir}/jetpac_native.so"
    echo "  built jetpac_native.so"
}
build_jetpac

# Sound and other assets live beside the library and are NOT built - they are
# what an import writes. Anything already installed is left alone so
# reinstalling a plugin never deletes a bundle's extracted assets.
for short_name in geometrywars geometrywars2 bubblebobble_native jetpac_native; do
    dest="${install_root}/${short_name}"
    mkdir -p "${dest}/sfx"
    cp "${out}/${short_name}/${short_name}.so" "${dest}/${short_name}.so"
    count=$(find "${dest}/sfx" -maxdepth 1 -iname '*.wav' | wc -l)
    echo "  installed ${short_name} -> ${dest} (${count} sample(s) in sfx/)"
done

echo "Done. Cues with no sample in sfx/ are synthesised at load."
