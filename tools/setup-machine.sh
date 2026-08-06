#!/usr/bin/env bash
# MANX test-machine setup. Run once on a Linux machine you want to test MANX
# multiplayer on:
#
#     bash tools/setup-machine.sh
#
# It opens remote access so builds can be pushed and logs read back, and
# reports what the machine looks like from the network. It does NOT open
# anything for MANX itself - multiplayer needs no firewall rule at all, which
# is the point of how discovery works (see docs/netplay.md).
#
# Everything here is idempotent: a second run changes nothing.
set -uo pipefail

# The public half of the key pair on the development machine. Only ever
# appended to your own ~/.ssh/authorized_keys; the private half never leaves
# the machine that made it.
PUBKEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGj8ZcfjenWNyfKybRCyv1uR/v4Mnem5UkEOJc20Fbv8 manx-push-from-cachyos'

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok()   { printf '   \033[32mok\033[0m   %s\n' "$*"; }
warn() { printf '   \033[33mnote\033[0m %s\n' "$*"; }
bad()  { printf '   \033[31mfail\033[0m %s\n' "$*"; }

if [[ $EUID -eq 0 ]]; then
    echo "Run this as your normal user, not root - the key has to go into your"
    echo "own account. It asks for sudo where it needs it."
    exit 1
fi

say "Machine"
USER_NAME=$(whoami)
echo "   user : $USER_NAME"
echo "   host : $(hostname)"
echo "   os   : $( (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || uname -s)"

say "SSH server"
if ! command -v sshd >/dev/null 2>&1 && [[ ! -x /usr/sbin/sshd ]]; then
    warn "not installed - installing"
    if   command -v pacman  >/dev/null; then sudo pacman -S --needed --noconfirm openssh
    elif command -v apt-get >/dev/null; then sudo apt-get update -qq && sudo apt-get install -y openssh-server
    elif command -v dnf     >/dev/null; then sudo dnf install -y openssh-server
    elif command -v zypper  >/dev/null; then sudo zypper --non-interactive install openssh
    else bad "unknown package manager - install openssh-server by hand"; fi
fi
# The unit is sshd on most distros and ssh on Debian/Ubuntu.
SSH_UNIT=""
for unit in sshd ssh; do
    if systemctl list-unit-files 2>/dev/null | grep -q "^${unit}\.service"; then
        SSH_UNIT=$unit; break
    fi
done
if [[ -n "$SSH_UNIT" ]]; then
    sudo systemctl enable --now "$SSH_UNIT" >/dev/null 2>&1
    if systemctl is-active --quiet "$SSH_UNIT"; then ok "$SSH_UNIT running"
    else bad "$SSH_UNIT would not start - check: systemctl status $SSH_UNIT"; fi
else
    bad "no ssh service unit found"
fi

# A connection that times out rather than being refused means a firewall is
# dropping it - exactly what a deny-by-default desktop firewall does.
say "Firewall"
if command -v ufw >/dev/null 2>&1 && sudo ufw status 2>/dev/null | grep -q "^Status: active"; then
    sudo ufw allow 22/tcp comment 'MANX remote access' >/dev/null 2>&1
    ok "ufw: allowed 22/tcp"
elif command -v firewall-cmd >/dev/null 2>&1 && sudo firewall-cmd --state >/dev/null 2>&1; then
    sudo firewall-cmd --add-service=ssh --permanent >/dev/null 2>&1
    sudo firewall-cmd --reload >/dev/null 2>&1
    ok "firewalld: allowed ssh"
else
    ok "no active ufw/firewalld - nothing to open"
fi
warn "nothing opened for MANX itself: multiplayer needs no firewall rule"

say "Remote access key"
mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
touch "$HOME/.ssh/authorized_keys" && chmod 600 "$HOME/.ssh/authorized_keys"
if grep -qF "${PUBKEY##* }" "$HOME/.ssh/authorized_keys" 2>/dev/null; then
    ok "key already present"
else
    printf '%s\n' "$PUBKEY" >> "$HOME/.ssh/authorized_keys"
    ok "key added to ~/.ssh/authorized_keys"
fi

say "Reachable at"
ADDRS=$(ip -o -4 addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
if [[ -z "$ADDRS" ]]; then
    bad "no IPv4 address - is this machine on the network?"
else
    for a in $ADDRS; do echo "   $USER_NAME@$a"; done
fi

say "MANX"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$HERE/build-clean/MANX"
if [[ -x "$BIN" ]]; then
    MISSING=$(ldd "$BIN" 2>/dev/null | grep 'not found')
    if [[ -n "$MISSING" ]]; then
        bad "the prebuilt binary will NOT run here - missing libraries:"
        echo "$MISSING" | sed 's/^/        /'
        echo "        Build on this machine instead:"
        echo "          cd $HERE && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
    else
        ok "prebuilt binary has every library it needs"
        echo "        run it with: $BIN"
    fi
else
    warn "no prebuilt binary at $BIN"
fi

say "Done"
echo "   Report the user@address line above and builds can be pushed and this"
echo "   machine's logs read directly."
