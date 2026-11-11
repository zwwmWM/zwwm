#!/usr/bin/env bash
# Builds zwwm for the distribution described by packaging/<dist>/ and writes its package.
# Usage: packaging/build.sh <dist> [output-dir]
#
# A recipe directory holds:
#   image       container image CI builds in
#   deps.sh     installs the build dependencies
#   patches/    *.patch applied (-p1) to a copy of the source before building
#   package.sh  wraps the staged install; reads the ZWWM_* variables exported below
set -euo pipefail

dist=${1:?usage: build.sh <dist> [output-dir]}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
recipe=$here/$dist
if [[ ! -f $recipe/package.sh ]]; then
  echo "build.sh: no recipe for '$dist' in $here" >&2
  exit 2
fi
out=$(realpath -m "${2:-dist}")

work=$(mktemp -d)
# Read-only source directories would otherwise survive the cleanup.
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
src=$work/src
mkdir -p "$src" "$out"

export ZWWM_DIST=$dist ZWWM_RECIPE=$recipe ZWWM_WORK=$work ZWWM_OUT=$out
export ZWWM_STAGE=$work/stage
ZWWM_VERSION=$(tr -d '[:space:]' <"$root/.version")
export ZWWM_VERSION

# The tree itself is never edited; distribution fixes live in the recipe's patches.
# Local build trees are skipped by what they contain, not by name (build-aux is source).
excludes=(--exclude=./.git --exclude='./result*')
for dir in "$root"/*/; do
  if [[ -e ${dir}build.ninja || -e ${dir}CMakeCache.txt ]]; then
    excludes+=(--exclude="./$(basename "$dir")")
  fi
done
tar -C "$root" "${excludes[@]}" -cf - . | tar -C "$src" -xf -
shopt -s nullglob
for patch_file in "$recipe"/patches/*.patch; do
  echo "applying $(basename "$patch_file")"
  patch -d "$src" -p1 -f -s -i "$patch_file"
done

CC=clang CXX=clang++ meson setup "$work/build" "$src" --buildtype=release --prefix=/usr
meson compile -C "$work/build"
DESTDIR=$ZWWM_STAGE meson install -C "$work/build" --no-rebuild --strip

bash "$recipe/package.sh"
ls -l "$out"
