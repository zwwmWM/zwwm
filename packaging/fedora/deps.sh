#!/bin/sh
set -e
dnf -y install \
  clang gcc-c++ meson ninja-build cmake python3 patch rpm-build \
  glslang-devel spirv-tools-devel qt6-qtbase-devel \
  'pkgconfig(vulkan)' 'pkgconfig(libdrm)' 'pkgconfig(gbm)' 'pkgconfig(libpng)' \
  'pkgconfig(libxml-2.0)' 'pkgconfig(xkbcommon)' 'pkgconfig(xcb)' \
  'pkgconfig(xcb-composite)' 'pkgconfig(xcb-xfixes)' 'pkgconfig(xcursor)' \
  'pkgconfig(libpipewire-0.3)' 'pkgconfig(libseat)' 'pkgconfig(libinput)' \
  'pkgconfig(libudev)' 'pkgconfig(fontconfig)' 'pkgconfig(freetype2)'
