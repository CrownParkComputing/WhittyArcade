#!/usr/bin/env bash
# Build the Stunt Car Racer MANX plugin.
#
# Produces libstuntcarracer.so in this directory. The host loads it
# via dlopen when it sees it in the games directory. No MANX source
# change required — the plugin is fully self-contained.
#
# Usage:  ./build.sh
#         cp libstuntcarracer.so ~/.local/share/manx/games/stuntcarracer/
#         ./MANX
#
# Cross-platform note: the build uses only POSIX C++ and the engine's
# own math. No OpenGL, no EGL, no platform-specific includes — the
# same source compiles on Linux + Android (with the NDK) without
# changes.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MANX_INCLUDE="$(cd "$HERE/../.." && pwd)/include"  # /home/jon/MANX/include
PORT_ROOT="$(cd "$HERE/../../../Projects/stunt-car-racer-linux" 2>/dev/null && pwd || true)"
# Project root = the stunt-car-racer-linux repo. The build assumes the
# same layout we used during development: MANX at /home/jon/MANX,
# port repo at /home/jon/Projects/stunt-car-racer-linux. Override
# with SC_PORT_ROOT=/path/to/stunt-car-racer-linux if your layout differs.
if [ -z "$PORT_ROOT" ] || [ ! -d "$PORT_ROOT" ]; then
    SC_PORT_ROOT="${SC_PORT_ROOT:-}"
    if [ -n "$SC_PORT_ROOT" ] && [ -d "$SC_PORT_ROOT" ]; then
        PORT_ROOT="$SC_PORT_ROOT"
    else
        echo "ERROR: cannot find stunt-car-racer-linux port repo." >&2
        echo "       Set SC_PORT_ROOT=/path/to/stunt-car-racer-linux" >&2
        exit 1
    fi
fi
ENGINE_FILE="$PORT_ROOT/src/3d_engine_linux.cpp"
RASTER_HDR="$PORT_ROOT/src/sc_rasterizer.h"
if [ ! -f "$ENGINE_FILE" ]; then
    echo "ERROR: $ENGINE_FILE not found" >&2
    exit 1
fi
if [ ! -f "$RASTER_HDR" ]; then
    echo "ERROR: $RASTER_HDR not found" >&2
    exit 1
fi

OUT="$HERE/libstuntcarracer.so"

CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2 -fPIC -fvisibility=hidden -Wall -Wextra}"
DEFINES="${DEFINES:--DMANX_PLUGIN=1}"
INCLUDES="-I$MANX_INCLUDE -I$PORT_ROOT/src -I$HERE"

set -x
"$CXX" $CXXFLAGS $DEFINES $INCLUDES \
    -shared -o "$OUT" \
    "$HERE/sc_plugin.cpp" \
    "$ENGINE_FILE" \
    "$PORT_ROOT/src/sc_rasterizer.cpp"
set +x

echo
echo "Built: $OUT ($(du -h "$OUT" | cut -f1))"
echo "Install with:"
echo "  cp $OUT ~/.local/share/manx/games/stuntcarracer/"
echo "Then launch MANX — the game appears in the launcher."
