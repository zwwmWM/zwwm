#!/usr/bin/env bash
set -euo pipefail

version=${ZWWM_VERSION/-/\~}
arch=$(dpkg --print-architecture)
elf=()
while IFS= read -r file; do
  [[ $(head -c 4 "$file" | tr -d '\0') == $'\x7fELF' ]] && elf+=("$file")
done < <(cd "$ZWWM_WORK" && find stage -type f)
# dpkg-shlibdeps only runs next to a debian/control file.
mkdir -p "$ZWWM_WORK/debian" "$ZWWM_STAGE/DEBIAN"
: >"$ZWWM_WORK/debian/control"
shlibs=$(cd "$ZWWM_WORK" && dpkg-shlibdeps -O --ignore-missing-info "${elf[@]}" | sed -n 's/^shlibs:Depends=//p')

control=$(<"$ZWWM_RECIPE/control.in")
control=${control//@VERSION@/$version}
control=${control//@ARCH@/$arch}
control=${control//@SHLIBS@/$shlibs}
printf '%s\n' "$control" >"$ZWWM_STAGE/DEBIAN/control"
(cd "$ZWWM_STAGE" && find etc -type f | sort | sed 's|^|/|') >"$ZWWM_STAGE/DEBIAN/conffiles"
dpkg-deb --root-owner-group --build "$ZWWM_STAGE" "$ZWWM_OUT/zwwm_${version}_${ZWWM_DIST}_${arch}.deb"
