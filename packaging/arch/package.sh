#!/usr/bin/env bash
set -euo pipefail

mkdir -p "$ZWWM_WORK/makepkg" "$ZWWM_WORK/pkgdest"
cp "$ZWWM_RECIPE/PKGBUILD" "$ZWWM_WORK/makepkg/"
cd "$ZWWM_WORK/makepkg"
makepkg=(env HOME="$ZWWM_WORK" PKGDEST="$ZWWM_WORK/pkgdest" ZWWM_VERSION="$ZWWM_VERSION"
         ZWWM_STAGE="$ZWWM_STAGE" makepkg --nodeps --force)
# makepkg refuses to run as root, which is what CI containers give us.
if [[ $EUID -eq 0 ]]; then
  chown -R nobody "$ZWWM_WORK"
  runuser -u nobody -- "${makepkg[@]}"
else
  "${makepkg[@]}"
fi
cp "$ZWWM_WORK"/pkgdest/*.pkg.tar.* "$ZWWM_OUT/"
