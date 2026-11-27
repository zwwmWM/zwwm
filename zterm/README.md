# zterm

Experimental Vulkan terminal for zwwm, built on zwayland and libzterm.
Configured in `.zw`, and a shared zwwm config can hold a `zterm = { ... }` block.

## Run

From the repository root:

```sh
nix run .#zterm
```

Add `-- --standalone` on other compositors, or `-- -e COMMAND` to run a program.
Needs Vulkan 1.3 and linux-dmabuf v4.

### Daemon

```sh
nix run .#zterm -- daemon
```

Keeps fonts and GPU resources warm for faster new windows.

## Configuration

Create `~/.config/zterm/config.zw`, or select a file with `--config FILE` or
`ZTERM_CONFIG`. CLI options win. All settings are in [config.zw](config.zw);
edits reload live.

`theme = "NAME"` loads a `.zw` or Ghostty theme from `~/.config/zterm/themes`,
`~/.config/ghostty/themes` or the installed theme dirs. Config colors override
the theme. `--list-themes` lists them.

<!-- config-check: skip -->
```zw
font = {
  mono = "JetBrainsMono Nerd Font Mono"
  symbols = "Symbols Nerd Font Mono"
  emoji = "Noto Color Emoji"
  size = 14
  weight = 400
}

rendering = { background-opacity-per-mille = 850 }

colors = {
  foreground = "#dce1e8"
  background = "#101319"
  selection = "#387ae673"
  red = "#e06c75"
  bright-red = "#ff7a85"
}

cursor = { shape = "beam" blink = true }
window = { padding-x = 10 padding-y = 10 }
```

Fonts also take `bold`, `italic` and `bold-italic`; all 16 ANSI colors are
settable.

On NixOS use `programs.zwwm.zterm.settings`; it writes `/etc/xdg/zterm/config.zw`.

## Controls

- Ctrl+Shift+R: reload config
- Ctrl+Shift+C / V: copy, paste
- Double/triple click: select word/line; Shift-drag in mouse-aware apps
- Shift+PageUp/PageDown: scroll
