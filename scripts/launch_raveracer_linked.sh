#!/usr/bin/env bash
# launch_raveracer_linked.sh — Spin up two MANX cabinets linked via
# the Namco System 22 C139 cabinet-to-cabinet link protocol.
#
# Usage:
#   ./scripts/launch_raveracer_linked.sh
#
# Requirements:
#   - MANX built at build/MANX
#   - Rave Racer ROM in Downloads/MANX-Roms/raverace.zip
#   - System 22 BIOS files (namcoc71.zip, namcoc74.zip) in the same dir
#
# What happens:
#   Cabinet 1 binds UDP 35112, sends to 127.0.0.1:35113
#   Cabinet 2 binds UDP 35113, sends to 127.0.0.1:35112
#   Both processes run side by side on the current desktop.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$PROJECT_DIR/build/MANX"
ROM_DIR="$HOME/Downloads/MANX-Roms"
ROM="$ROM_DIR/raverace.zip"
BIOS="$ROM_DIR"  # namcoc71.zip + namcoc74.zip live beside the ROM

# ---- preflight checks -------------------------------------------------
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: MANX binary not found at $BINARY" >&2
    echo "Build it first: cd $PROJECT_DIR/build && make -j$(nproc)" >&2
    exit 1
fi

if [[ ! -f "$ROM" ]]; then
    echo "ERROR: Rave Racer ROM not found at $ROM" >&2
    exit 1
fi

echo "=== MANX Rave Racer Linked-Cabinet Launcher ==="
echo "ROM:      $ROM"
echo "Binary:   $BINARY"
echo ""

# ---- launch cabinet 1 (Player 1) --------------------------------------
echo "[1/2] Starting Cabinet 1 (Player 1) on UDP 35112 → 35113..."
"$BINARY" \
    --cabinet-node 1 \
    --network-cabinet \
    "$ROM" \
    "$BIOS" &
CAB1_PID=$!

# Give cabinet 1 a moment to bind its socket before cabinet 2 starts.
sleep 1

# ---- launch cabinet 2 (Player 2) --------------------------------------
echo "[2/2] Starting Cabinet 2 (Player 2) on UDP 35113 → 35112..."
echo ""
echo "Both cabinets are running. Press Ctrl+C to stop."
echo ""

"$BINARY" \
    --cabinet-node 2 \
    --network-cabinet \
    "$ROM" \
    "$BIOS" &
CAB2_PID=$!

# ---- wait for both to finish ------------------------------------------
cleanup() {
    echo ""
    echo "Shutting down..."
    kill "$CAB1_PID" 2>/dev/null || true
    kill "$CAB2_PID" 2>/dev/null || true
    wait "$CAB1_PID" 2>/dev/null || true
    wait "$CAB2_PID" 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT INT TERM

wait "$CAB1_PID" "$CAB2_PID"
