#!/usr/bin/env bash
set -euo pipefail

# xdg-desktop-portal.service requires graphical-session.target; without it screencast never starts.
start_session_target() {
  systemctl --user show-environment >/dev/null 2>&1 || return 1
  local unit_dir="${XDG_RUNTIME_DIR:-/run/user/$UID}/systemd/user"
  mkdir -p "$unit_dir"
  cat >"$unit_dir/zwwm-session.target" <<'EOF'
[Unit]
Description=zwwm compositor session
BindsTo=graphical-session.target
Wants=graphical-session-pre.target
After=graphical-session-pre.target
EOF
  systemctl --user daemon-reload
  systemctl --user reset-failed xdg-desktop-portal.service 2>/dev/null || true
  systemctl --user start zwwm-session.target
}

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
portal_root="${XDG_DATA_HOME:-$HOME/.local/share}/xdg-desktop-portal"
portal_dir="$portal_root/portals"

mkdir -p "$portal_dir"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm.portal" "$portal_dir/zwwm.portal"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm-gtk.portal" "$portal_dir/zwwm-gtk.portal"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm-portals.conf" "$portal_root/zwwm-portals.conf"

ZWWM_CONFIG="$HOME/.config/zwwm/config.zw"

session=(env -u WAYLAND_DISPLAY -u WAYLAND_DEBUG
  XDG_CURRENT_DESKTOP=zwwm
  XDG_SESSION_DESKTOP=zwwm
  XDG_SESSION_TYPE=wayland
  NIX_XDG_DESKTOP_PORTAL_DIR="$portal_dir"
  ZWWM_CONFIG="$ZWWM_CONFIG"
  nix develop "$root" --no-write-lock-file --command
  bash -c '
    root=$1
    shift
    if [[ ! -f "$root/bld/build.ninja" ]]; then
      CC=clang CXX=clang++ meson setup "$root/bld" "$root" --buildtype=release
    fi
    meson compile -C "$root/bld" zwwm xdg-desktop-portal-zwwm
    exec "$root/bld/zwwm" \
      --portal-helper "$root/bld/xdg-desktop-portal-zwwm" "$@"
  ' zwwm-launch "$root" "$@")

if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" && ! -S "${XDG_RUNTIME_DIR:-}/bus" ]] &&
   command -v dbus-run-session >/dev/null 2>&1; then
  exec dbus-run-session -- "${session[@]}"
fi
if start_session_target; then
  status=0
  "${session[@]}" || status=$?
  systemctl --user stop zwwm-session.target
  exit "$status"
fi
exec "${session[@]}"
