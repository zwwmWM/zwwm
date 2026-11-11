#!/bin/sh
set -e
pkg install -y \
  bash meson ninja cmake pkgconf python3 \
  libxml2 vulkan-headers vulkan-loader glslang spirv-tools libdrm mesa-libs png \
  libxkbcommon libxcb libXcursor pipewire qt6-base seatd libinput libudev-devd \
  evdev-proto fontconfig freetype2
# Runtime packages are installed too so package.sh can record their versions.
pkg install -y $(cat "$(dirname "$0")/depends")
