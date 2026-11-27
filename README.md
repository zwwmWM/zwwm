# zwwm
zwwm is a C++23 post-Wayland compositor with a Wayland stack rebuilt from the ground up —
even though in 2026 everyone seems to be rewriting everything in Rust.

Daily driven for months by me and a few others; it is stable.

## Features

- Atomic DRM/KMS and nested Wayland backends
- Vulkan 1.3 renderer with zero-copy DMA-BUF clients and implicit sync
- libinput and XKB keyboard support
- Touchscreen input and a native virtual-keyboard protocol for on-screen keyboard clients
- Master-stack tiling, floating windows, fullscreen, tags and a tag overview
- Xwayland windows integrated into layout, focus, actions, and clipboard
- Custom shaders
- Screencopy and an XDG desktop portal backend
- Live config reload and `zwwmctl` control

## Build

### Nix

```sh
nix build
nix run
```

The flake also exports `packages.<system>.zwwm-no-xwayland` and a NixOS module:

```nix
{
  inputs.zwwm.url = "github:zwwmWM/zwwm";

  outputs = { nixpkgs, zwwm, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        zwwm.nixosModules.default
        {
          programs.zwwm = {
            enable = true;
            xwayland.enable = true;
            zterm = {
              enable = true;
              settings = {
                font = { mono = "JetBrainsMono Nerd Font Mono"; size = 14; };
                cursor.shape = "beam";
              };
            };
          };
        }
      ];
    };
  };
}
```

`settings` and `zterm.settings` take `.zw` settings as Nix attrs; `extraConfig`
appends raw `.zw`.

`zwayland`, `zwayland-scanner` and `zwwm-protocols` are also exported as
packages and through `overlays.default`, with CMake and pkg-config files.

### CMake

```sh
nix develop
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release \
  -DXWAYLAND_ENABLE=ON
cmake --build build -j2
sudo "$(command -v cmake)" --install build
```

### Meson

```sh
nix develop
CC=clang CXX=clang++ meson setup build-meson --buildtype=release --prefix=/usr -Dxwayland=true
meson compile -C build-meson -j2
sudo meson install -C build-meson
```

Turn off `XWAYLAND_ENABLE` / `-Dxwayland` to build without Xwayland.

## Run

Start `zwwm-session` from a TTY or display manager. It needs atomic KMS and
works without systemd: seat access goes through libseat (`LIBSEAT_BACKEND=logind`
for elogind) and a D-Bus session is started if none exists.
The default config is `zwwm/data/config.zw`.

## Control

```sh
zwwmctl status
zwwmctl outputs -j
zwwmctl clients -j
zwwmctl camera -j
zwwmctl keyboard -j
zwwmctl dispatch tag 2
zwwmctl dispatch overview ""     # toggle; or on, off
zwwmctl reload
zwwmctl rebuild-switch-shaders
zwwmctl setcursor "Bibata-Modern-Classic" 24
```

Configuration and shaders are documented in [CONFIG.md](CONFIG.md).

## Community Projects

### [Just Enough Shell (JES)](https://github.com/ORFLEM/just_enough_shell)

Modular desktop shell with native zwwm support.

[![Just Enough Shell lock screen](https://raw.githubusercontent.com/ORFLEM/dots/main/images/10.webp)](https://github.com/ORFLEM/just_enough_shell)

[Desktop preview](https://raw.githubusercontent.com/ORFLEM/dots/main/images/7.webp)
· [Screenshot gallery](https://github.com/ORFLEM/just_enough_shell/blob/main/documentation/eng.md)

### [blxshell / dots](https://github.com/binarylinuxx/dots)

Material You Quickshell shell with a native zwwm plugin.

[![blxshell desktop with zwwm integration](https://raw.githubusercontent.com/binarylinuxx/dots/main/20261011_10h30m37s_grim.png)](https://github.com/binarylinuxx/dots)

## Components

- `zwwm/`: compositor, backends, input, layout, Xwayland
- [`zwwm-renderer/`](zwwm-renderer/README.md): scene and Vulkan rendering
- [`zwwm-vlk/`](zwwm-vlk/README.md): Vulkan device and DMA-BUF import
- `zwayland/`: Wayland client/server runtime
- `zwayland-scanner/`: binding generator for Wayland XML and [zxml](zwwm-protocols/ZXML.md)
- [`zwwm-protocols/`](zwwm-protocols/README.md): protocols and generated bindings
- [`zw-lang/`](zw-lang/README.md): config language
- [`zterm/`](zterm/README.md): GPU terminal
- [`xdg-desktop-portal-zwwm/`](xdg-desktop-portal-zwwm/README.md): desktop portal
- `zwwmctl/`: command-line control

Clang is the only supported compiler; GCC fixes are low priority.

## License

[BSD 3-Clause](LICENSE).
