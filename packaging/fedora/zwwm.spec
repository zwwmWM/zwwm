# Wraps the install staged by packaging/build.sh, which defines
# zwwm_version, zwwm_stage and zwwm_files.
Name:           zwwm
Version:        %{zwwm_version}
Release:        1%{?dist}
Summary:        Wayland compositor with tiling and endless-canvas layouts
License:        BSD-3-Clause
URL:            https://github.com/zwwmWM/zwwm
Requires:       xorg-x11-server-Xwayland
Requires:       xdg-desktop-portal
Recommends:     xdg-desktop-portal-gtk

%global debug_package %{nil}

%description
zwwm is a Vulkan Wayland compositor built on its own zwayland runtime.

%install
mkdir -p %{buildroot}
cp -a %{zwwm_stage}/. %{buildroot}/

%files -f %{zwwm_files}
