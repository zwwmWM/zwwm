#!/usr/bin/env bash
set -euo pipefail

manifest=$ZWWM_WORK/+MANIFEST
plist=$ZWWM_WORK/plist
(cd "$ZWWM_STAGE" && find . \( -type f -o -type l \) | sort | sed 's|^\.||') >"$plist"
{
  cat "$ZWWM_RECIPE/manifest.in"
  echo "version: \"${ZWWM_VERSION//-/.}\""
  echo "deps: {"
  # Each runtime package is pinned to the version installed in the build VM.
  while IFS= read -r name; do
    [[ -n $name ]] && pkg query "  \"%n\": {origin: \"%o\", version: \"%v\"}," "$name"
  done <"$ZWWM_RECIPE/depends"
  echo "}"
} >"$manifest"
pkg create -M "$manifest" -p "$plist" -r "$ZWWM_STAGE" -o "$ZWWM_OUT"
