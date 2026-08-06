#!/usr/bin/env bash
# Push this tree somewhere that will build it, using the token in
# ~/cpcgithub.txt rather than whatever credential git happens to have.
#
# Why this exists: the origin in this checkout points at
# RetroRecompilations/MANXArcade, which is a different project - a modular
# split built on MANXFramework, created after this tree and sharing not one
# commit with it. Pushing there fails on permissions, and would be wrong even
# with permissions. Meanwhile the work in this folder exists nowhere else.
#
# So this pushes the current branch to a repository the CrownParkComputing
# token can actually write to, which also starts the Windows and Linux
# builds: .github/workflows/windows.yml triggers on a push to any branch.
#
#   scripts/push-and-build.sh                     # to CrownParkComputing/MANX
#   scripts/push-and-build.sh CrownParkComputing/MANXArcade
#   scripts/push-and-build.sh --dry-run           # say what it would do
#
# The token is passed to git through a credential helper reading an
# environment variable, so it never lands in .git/config, in a remote URL, or
# in the process list.
set -euo pipefail

TOKEN_FILE="${MANX_TOKEN_FILE:-$HOME/cpcgithub.txt}"
TARGET="CrownParkComputing/MANX"
DRY=""

for argument in "$@"; do
  case "$argument" in
    --dry-run) DRY="--dry-run" ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    */*)       TARGET="$argument" ;;
    *)         echo "Unrecognised argument: $argument" >&2; exit 2 ;;
  esac
done

if [ ! -r "$TOKEN_FILE" ]; then
  echo "No token at $TOKEN_FILE." >&2
  echo "Put a GitHub token with Contents:write on $TARGET there." >&2
  exit 1
fi

# Strip the byte-order mark and any stray newline: a token pasted out of a
# Windows editor carries both, and the failure it produces is a 401 that says
# nothing about why.
MANX_GH_TOKEN="$(tr -d '\r\n\357\273\277' < "$TOKEN_FILE")"
export MANX_GH_TOKEN

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
COMMITS="$(git rev-list --count HEAD)"

echo "Repository : $(pwd)"
echo "Branch     : $BRANCH  ($COMMITS commits)"
echo "Pushing to : https://github.com/$TARGET"

if [ -n "$(git status --porcelain)" ]; then
  echo
  echo "Uncommitted changes are present. They will NOT be pushed:"
  git status --short | sed 's/^/  /'
fi

# Whoever the token is, said out loud before anything is written.
WHO="$(curl -s -H "Authorization: Bearer $MANX_GH_TOKEN" \
        https://api.github.com/user | sed -n 's/.*"login": "\([^"]*\)".*/\1/p' \
        | head -1)"
echo "Token is   : ${WHO:-unknown}"

CAN_PUSH="$(curl -s -H "Authorization: Bearer $MANX_GH_TOKEN" \
             "https://api.github.com/repos/$TARGET" \
             | tr ',' '\n' | sed -n 's/.*"push": *\(true\|false\).*/\1/p' \
             | head -1)"
if [ "$CAN_PUSH" != "true" ]; then
  echo
  echo "That token cannot write to $TARGET." >&2
  echo "Either pick a repository it can - CrownParkComputing/MANX," >&2
  echo "MANXArcade or MANXFramework - or give it Contents:write there." >&2
  exit 1
fi

echo
git -c credential.helper='!f() { echo username=x-access-token; echo "password=$MANX_GH_TOKEN"; }; f' \
    push $DRY "https://github.com/$TARGET.git" "$BRANCH:$BRANCH"

if [ -n "$DRY" ]; then
  echo
  echo "Dry run only - nothing was pushed."
  exit 0
fi

cat <<INFO

Pushed. windows.yml and the Linux workflow trigger on a push to any branch,
so both builds should now be running:

  https://github.com/$TARGET/actions

A cold Windows build is twenty to forty minutes - most of it installing the
MSYS2 UCRT64 toolchain. The artifact is MANX-windows-x86_64.

To publish both platforms as a downloadable release, tag it:

  git tag -a v0.1.0 -m "MANX v0.1.0"
  git -c credential.helper='!f() { echo username=x-access-token; echo "password=\$MANX_GH_TOKEN"; }; f' \\
      push "https://github.com/$TARGET.git" v0.1.0

The website prefers a GitHub release over the hand-built Linux tarball it is
serving now, so the download buttons change over by themselves.
INFO
