#!/bin/sh
set -e
pacman -Syu --noconfirm --needed \
  clang meson ninja cmake pkgconf python patch \
  libxml2 vulkan-headers vulkan-icd-loader glslang spirv-tools libdrm mesa libpng \
  libxkbcommon libxcb libxcursor pipewire qt6-base seatd libinput fontconfig \
  freetype2 systemd-libs
