# zwwm-protocols

All protocols are [zxml](ZXML.md): project ones under `protocols/`, Wayland
1.26.0 and wayland-protocols 1.49 under `protocols/upstream/`. `zwayland-scanner`
generates C++ bindings from them at build time. Generated XML for the
project protocols is installed to `share/zwwm/protocols/`.

## Project protocols

- `zwwm-tags-unstable-v1`: tag observation and control
- `zwwm-screencopy-view-unstable-v1`: output and region capture (portal only)
- `ext-zwwm-toplevels-unstable-v1`: toplevel list and capture (portal only)
- `ext-zwwm-manager-v1`: compositor state, control and events (`zwwmctl`)
- `zwwm-client-connection-v1`: preconnected sockets and named daemon channels
- `zwwm-data-control-v1`: clipboard control
- `zwwm-layer-shell-v1`: layer surfaces, also served as `zwlr_layer_shell_v1` v4
- `xwlr-layer-shell-v1`: extra layer hints: priority, opacity, exclusive edge
- `zwwm-focus-grab-v1`: dismissible keyboard grabs for panels
- `zwwm-decoration-manager-v1`: server-side decoration policy
- `zwwm-virtual-keyboard-v1`: key injection using the compositor keymap
- `zwwm-window-manager-v1`: runtime config, window control, policy ownership
- `zwwm-layout-manager-v1`: external tiling layouts
- `zwwm-xwayland-shell-v1`: Xwayland window to surface association

`zwlr_screencopy_manager_v1` stays public for tools like `grim`.

## Virtual keyboard

`create_keyboard(seat)` sends the current XKB keymap and modifiers, and again
whenever they change. Send `key(time, evdev_code, pressed)` and a matching
release; modifiers are ordinary keys. Input is ignored while the session is
locked. Use a layer surface with keyboard interactivity `none` so tapping the
keyboard doesn't steal focus.

## External window managers

`zwwm_window_manager_v1` and `zwwm_layout_manager_v1` let a separate process
supply config, focus policy and layout while zwwm keeps rendering and protocol
handling.

### Configuration

`create_configuration` clones the running config into a draft. Set properties
and collections, then `commit`; the result reports `applied(revision)` or
`failed(message)`. A failed or abandoned draft changes nothing. Changes are not
written to `config.zw`.

Property names follow `config.zw` paths (`animations.duration-ms`,
`input.focus-mode`, `keyboard.layout`, `overview.layout`, ...) with the same
ranges as [CONFIG.md](../CONFIG.md). Collections cover bindings, window rules,
layer rules, output scale/transform and shaders.

### Policy (version 2)

- `get_state`: windows, outputs and tags. Window IDs live as long as the surface.
- `changed`: coalesced change notice; fetch a new snapshot.
- `window_action`, `assign_window`, `select_tag`, `dispatch`: control, each
  answered by a `zwwm_window_result_v1`.

`claim_policy` makes one client the focus owner. zwwm stops auto-focusing and
forwards pointer, touch and gesture input, `external` bindings, and app
fullscreen/maximize/minimize/move/resize requests as `window_request`. Floating
dialogs, popups and interactive floating move/resize stay native. Releasing or
disconnecting restores built-in behavior.

### Layout

Create a provider to place tiled windows on every output. For each `request`:
collect `window` events until `done`, send one `place` per window in global
logical coordinates, then `commit(serial, animate)`. Incomplete or stale batches
fail and leave the previous layout. `request_layout` asks for a new pass. When
the provider goes away, built-in tiling returns.

```text
bind window manager v2 and layout manager v1
claim policy, commit config, create layout provider

changed        -> get_state -> window actions
binding/input  -> update state -> request_layout
layout done    -> place every window -> commit
window_request -> accept or ignore
```
