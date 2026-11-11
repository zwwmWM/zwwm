#!/usr/bin/env bash
set -euo pipefail

files=$ZWWM_WORK/files.list
(cd "$ZWWM_STAGE" && find usr \( -type f -o -type l \) | sort | sed 's|^|/|') >"$files"
(cd "$ZWWM_STAGE" && find etc -type f | sort | sed 's|^|%config(noreplace) /|') >>"$files"
rpmbuild -bb \
  --define "_topdir $ZWWM_WORK/rpm" \
  --define "zwwm_version ${ZWWM_VERSION/-/\~}" \
  --define "zwwm_stage $ZWWM_STAGE" \
  --define "zwwm_files $files" \
  "$ZWWM_RECIPE/zwwm.spec"
cp "$ZWWM_WORK"/rpm/RPMS/*/*.rpm "$ZWWM_OUT/"
