#!/usr/bin/env bash
# Builds zwwm with Meson and wraps the staged install in a native package.
# Usage: build-aux/package.sh deb|rpm|pacman [output-dir]
# PKG_LABEL (optional) names the distribution in .deb file names.
set -euo pipefail

format=${1:?usage: package.sh deb|rpm|pacman [output-dir]}
out=$(realpath -m "${2:-dist}")
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=$(tr -d '[:space:]' <"$root/.version")
url=https://github.com/zwwmWM/zwwm
summary="Wayland compositor with tiling and endless-canvas layouts"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage=$work/stage

CC=clang CXX=clang++ meson setup "$work/build" "$root" --buildtype=release --prefix=/usr -Dxwayland=true
meson compile -C "$work/build"
DESTDIR=$stage meson install -C "$work/build" --no-rebuild --strip
mkdir -p "$out"

config_files() { (cd "$stage" && find etc -type f | sort); }

case $format in
  deb)
    deb_version=${version/-/\~}
    deb_arch=$(dpkg --print-architecture)
    elf=()
    while IFS= read -r file; do
      [[ $(head -c 4 "$file" | tr -d '\0') == $'\x7fELF' ]] && elf+=("$file")
    done < <(cd "$work" && find stage -type f)
    # dpkg-shlibdeps only runs next to a debian/control file.
    mkdir -p "$work/debian" "$stage/DEBIAN"
    : >"$work/debian/control"
    shlibs=$(cd "$work" && dpkg-shlibdeps -O --ignore-missing-info "${elf[@]}" | sed -n 's/^shlibs:Depends=//p')
    config_files | sed 's|^|/|' >"$stage/DEBIAN/conffiles"
    cat >"$stage/DEBIAN/control" <<EOF
Package: zwwm
Version: $deb_version
Architecture: $deb_arch
Maintainer: zwwmWM <zwwmWM@users.noreply.github.com>
Section: x11
Priority: optional
Homepage: $url
Depends: $shlibs, xwayland, xdg-desktop-portal
Recommends: xdg-desktop-portal-gtk
Description: $summary
 zwwm is a Vulkan Wayland compositor built on its own zwayland runtime.
EOF
    dpkg-deb --root-owner-group --build "$stage" \
      "$out/zwwm_${deb_version}${PKG_LABEL:+_$PKG_LABEL}_${deb_arch}.deb"
    ;;
  rpm)
    (cd "$stage" && find usr \( -type f -o -type l \) | sort | sed 's|^|/|') >"$work/files.list"
    config_files | sed 's|^|%config(noreplace) /|' >>"$work/files.list"
    cat >"$work/zwwm.spec" <<EOF
Name: zwwm
Version: ${version/-/\~}
Release: 1%{?dist}
Summary: $summary
License: BSD-3-Clause
URL: $url
Requires: xorg-x11-server-Xwayland
Requires: xdg-desktop-portal
Recommends: xdg-desktop-portal-gtk

%global debug_package %{nil}

%description
zwwm is a Vulkan Wayland compositor built on its own zwayland runtime.

%install
mkdir -p %{buildroot}
cp -a $stage/. %{buildroot}/

%files -f $work/files.list
EOF
    rpmbuild -bb --define "_topdir $work/rpm" "$work/zwwm.spec"
    cp "$work"/rpm/RPMS/*/*.rpm "$out/"
    ;;
  pacman)
    mapfile -t backup < <(config_files)
    mkdir -p "$work/pkg" "$work/out"
    cat >"$work/pkg/PKGBUILD" <<EOF
pkgname=zwwm
pkgver=${version//-/_}
pkgrel=1
pkgdesc="$summary"
arch=('$(uname -m)')
url="$url"
license=('BSD-3-Clause')
depends=(vulkan-icd-loader glslang libdrm mesa libpng libxkbcommon libxcb libxcursor pipewire qt6-base
         seatd libinput fontconfig freetype2 systemd-libs xorg-xwayland xdg-desktop-portal)
optdepends=('xdg-desktop-portal-gtk: file chooser and settings portals')
backup=(${backup[*]})
options=('!strip' '!debug')
package() { cp -a "$stage/." "\$pkgdir/"; }
EOF
    # makepkg refuses to run as root, which is what CI containers give us.
    if [[ $EUID -eq 0 ]]; then
      chown -R nobody "$work"
      (cd "$work/pkg" && runuser -u nobody -- env HOME="$work" PKGDEST="$work/out" makepkg --nodeps --force)
    else
      (cd "$work/pkg" && PKGDEST="$work/out" makepkg --nodeps --force)
    fi
    cp "$work"/out/*.pkg.tar.* "$out/"
    ;;
  *)
    echo "package.sh: unknown format '$format'" >&2
    exit 2
    ;;
esac

ls -l "$out"
