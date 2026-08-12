#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
exec /home/jon/recomp-cookbook/builders/manx.sh "$root" "${1:-build-cookbook}"
