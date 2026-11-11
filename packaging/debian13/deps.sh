#!/bin/sh
set -e
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  clang meson ninja-build cmake pkg-config python3 patch dpkg-dev \
  libxml2-dev libvulkan-dev glslang-dev spirv-tools libdrm-dev libgbm-dev libpng-dev \
  libxkbcommon-dev libxcb1-dev libxcb-composite0-dev libxcb-xfixes0-dev libxcursor-dev \
  libpipewire-0.3-dev qt6-base-dev libseat-dev libinput-dev libfontconfig-dev \
  libfreetype-dev libudev-dev
