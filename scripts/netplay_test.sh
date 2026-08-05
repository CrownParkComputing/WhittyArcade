#!/bin/bash
# Two cabinets, one machine, lockstep netplay - the two-computer setup with
# both ends on this desktop. Placed side by side so both are visible.
#
#   scripts/netplay_test.sh [rom]
# Defaults to Galaxian.

ROM="${1:-$HOME/.local/share/MANX/roms/galaxian/galaxian.zip}"
LOGS="${TMPDIR:-/tmp}/manx-netplay"
mkdir -p "$LOGS"
cd "$(dirname "$0")/.." || exit 1
[ -f "$ROM" ] || { echo "No ROM at $ROM"; exit 1; }

pkill -f "build/MANX --network-cabinet" 2>/dev/null
sleep 1
# Floating, so the compositor does not tile or stack the two cabinets.
hyprctl keyword windowrule "float, class:^([Ww]hitty[Aa]rcade)$" >/dev/null 2>&1

./build/MANX --network-cabinet --cabinet-node 1 --pair-port-base 35112 \
    "$ROM" > "$LOGS/player1.log" 2>&1 &
P1=$!
sleep 3
./build/MANX --network-cabinet --cabinet-node 2 --pair-port-base 35112 \
    "$ROM" > "$LOGS/player2.log" 2>&1 &
P2=$!
sleep 6

# Half the display each. Read the real width so this is not pinned to one
# monitor size.
WIDTH=$(hyprctl monitors -j 2>/dev/null |
        python3 -c "import json,sys; m=json.load(sys.stdin); print(m[0]['width'] if m else 3840)" \
        2>/dev/null || echo 3840)
HALF=$((WIDTH / 2))
WS=$(hyprctl activeworkspace -j 2>/dev/null |
     python3 -c "import json,sys; print(json.load(sys.stdin)['id'])" 2>/dev/null || echo 1)
for spec in "$P1 0" "$P2 $HALF"; do
    set -- $spec
    # Onto the workspace being looked at: a cabinet placed correctly but on
    # another workspace, or left behind the terminal that started it, reads
    # as "nothing opened".
    hyprctl dispatch movetoworkspacesilent "$WS,pid:$1" >/dev/null 2>&1
    hyprctl dispatch setfloating "pid:$1" >/dev/null 2>&1
    hyprctl dispatch resizewindowpixel "exact $((HALF - 20)) 1400,pid:$1" >/dev/null 2>&1
    hyprctl dispatch movewindowpixel "exact $2 20,pid:$1" >/dev/null 2>&1
    hyprctl dispatch alterzorder top,pid:$1 >/dev/null 2>&1
done
# Focus Player 1 so the keyboard goes somewhere sensible to start with.
hyprctl dispatch focuswindow "pid:$P1" >/dev/null 2>&1

# Report what the compositor actually did, rather than assuming it worked.
sleep 1
hyprctl clients -j 2>/dev/null | P1=$P1 P2=$P2 python3 -c "
import json, os, sys
want = {int(os.environ['P1']): 'Player 1', int(os.environ['P2']): 'Player 2'}
seen = {}
for w in json.load(sys.stdin):
    if w.get('pid') in want:
        seen[w['pid']] = w
for pid, name in want.items():
    w = seen.get(pid)
    if w:
        print(f"  {name}: at {w['at']} size {w['size']} workspace {w['workspace']['id']}")
    else:
        print(f"  {name}: NO WINDOW - check the log")
" 2>/dev/null

echo "Player 1: pid $P1   (left)"
echo "Player 2: pid $P2   (right)"
echo
echo "To start a two-player alternating game:"
echo "  1. Insert a coin on EACH cabinet   (coin key, default 5)"
echo "  2. Press START on the RIGHT cabinet to begin two-player"
echo "  3. Players then alternate turns, each driving their own screen"
echo
echo "Logs: $LOGS/player1.log  $LOGS/player2.log"
echo "Watch for sync trouble with:"
echo "  grep -i desync $LOGS/player*.log"
echo
echo "Stop both:  pkill -f 'build/MANX --network-cabinet'"
