# Changelog

All notable changes to zwwm are documented here.

## Unreleased

### Highlights

- Replaced the OpenGL ES/EGL renderer with Vulkan 1.3. `zwwm-vlk` replaces
  `zwwm-egl` and provides device selection, DMA-BUF import, and nested
  presentation.
- Direct sessions render into GBM scanout buffers imported into Vulkan, with
  modifiers negotiated against the primary plane, and attach implicit-sync
  fences so KMS and clients wait on the GPU instead of the CPU.
- Shaders are now Vulkan GLSL 4.50 against a new `zwwm.glsl` ABI: per-draw
  uniform block, bindless texture table, config blocks, and specialization
  constants. **Breaking:** OpenGL ES user shaders must be ported (see
  CONFIG.md); the packaged defaults are updated.
- Nested sessions advertise the rendering GPU in linux-dmabuf feedback.
- New `ZWWM_VULKAN_VALIDATION` and `ZWWM_VULKAN_DEVICE` environment variables.
- New optional `shadow` shader role draws a quad under each toplevel; the
  packaged `shadow.frag` renders soft Gaussian-style shadows.
- `zwwm.toplevel_rect` is now in output pixels on scaled and rotated outputs.
- zterm renders with Vulkan through `zwwm-vlk`: glyph atlases are array images
  filled by batched uploads, and its shaders are compiled to SPIR-V at build time.
- `zwwm-vlk` builds and installs standalone (pkg-config `zwwm-vlk`, flake package
  `zwwm-vlk`).
- Xwayland: `set_serial` applies on commit, `xwayland_shell_v1` is hidden from
  other clients, property changes refresh in per-window batches, floating state
  survives remaps, and clipboard sends above 64 KiB use INCR. Crashes restart
  with backoff, and the XWM adds `WM_STATE`, `_NET_FRAME_EXTENTS`, and
  `_NET_CLOSE_WINDOW`.

### Direct backend

- The cursor is shown on the KMS cursor plane, so pointer motion no longer
  composes a frame and a visible cursor no longer blocks direct scanout.
  Rotated outputs, cursors larger than the plane, and drivers that reject the
  plane keep the composited cursor.

### FreeBSD

- **Experimental:** FreeBSD builds through `-Dplatform=freebsd` (Meson) or
  `ZWWM_PLATFORM=freebsd` (CMake), which apply `freebsd/patches` at build time.
  It has not had a real test drive on FreeBSD hardware yet; expect breakage and
  please report what you find.

### zwayland

Same Wayland wire protocol and public API; the internals are reworked.

- Events are batched and flushed once per client per loop iteration, before the
  loop sleeps. `EPOLLOUT` is armed only while a socket is backed up, instead of
  one `epoll_ctl` and an extra wakeup per event.
- Input and output use contiguous buffers: reading no longer allocates per
  `recvmsg`, messages split across reads are not copied, and loopback
  connections hand buffers over without copying.
- `MessageBuilder` keeps typical messages in 256 bytes of inline storage, and
  scanner-generated senders encode straight into the connection.
- Resources and proxies live in id-indexed tables instead of hash maps of
  `shared_ptr`; objects destroyed mid-dispatch are freed once it unwinds.
- The protocol signature decides how many fds a message carries: unread fds
  are closed, and a missing fd is a protocol error.
- The event loop drives all timers from one timerfd, runs idle callbacks in
  FIFO order, and adds `add_prepare()` and single-iteration `dispatch()`.
- `Signal::emit` no longer allocates.
- **Behavior:** clients with more than 4 MiB of unsent events are disconnected
  (`Display::set_max_client_buffer`). The client library reuses object ids
  after `delete_id`, and server ids are reused only after a short quarantine.

### Portal

- **Behavior:** the Settings portal no longer forwards to xdg-desktop-portal-gtk.
  It answers only `org.freedesktop.appearance` `color-scheme`, fixed to prefer
  dark, so GTK interface settings (fonts, cursor theme, accent colour) are no
  longer visible through the portal.

### Mobile

- text-input-v3 relays to input-method-v2 (including the keyboard grab), and
  virtual-keyboard-v1 is supported, so squeekboard and wvkbd work without patches.
  Input-method popup surfaces are accepted but not drawn yet.
- `ZWWM_DRM_DEVICES` allow-list for the direct backend.
- Compositor-owned edge swipes through the new `zwwm-edge-swipe-v1` protocol.
- A toplevel's first configure now waits for the client's batch, so the preferred
  fractional scale arrives first; layer surfaces get it before their configure too.
- Built-in panels that report an unknown connector status are used, and outputs
  are re-probed with backoff until one lights up.
- `mobile.windows` policy: full-area apps, one visible per tag, fitted and centred
  dialogs.
- wlr-output-power-management and idle-inhibit; powered-off outputs ignore touch.
- Frame callbacks skip hidden apps and wallpaper or home layers covered by an
  opaque app.
- Direct scanout of fullscreen opaque DMA-BUF apps, releasing buffers only after
  they leave the screen.
- Bind flags `release`, `tap`, `hold`, `repeat` and `locked`, with `mobile.hold-ms`.

## 0.1.3-alpha.1 - 2026-09-23
nothing really interesting another bug fixes

### Highlights
- geometry for endless canvas now fractional preserving clusters intact without visual drift on panning zooming

## 0.1.2-alpha.1 - 2026-09-22

This is intended to become zwwm's first minimally stable release: a usable
baseline for regular installation and testing while configuration, private
protocols, and internal APIs remain subject to change.

### Highlights

- Reimplemented endless-canvas camera zoom with frame-driven smoothing, stable
  viewport ownership, centered scaling, and clean interaction with canvas pan.
- Restricted canvas tag transitions to windows visible through each tag's
  actual camera position and zoom, preventing off-screen canvas content from
  entering the transition.
- Added `zwwmctl listen`, which streams live JSON snapshots containing clients,
  active tags, camera state, and the effective keyboard layout.
- Extended the manager protocol to version 6 with combined live snapshots and
  camera and keyboard event subscriptions.
- Made Clang the required C and C++ compiler in both CMake and Meson, matching
  the project's supported-toolchain policy.

### Stability

This release establishes a more dependable runtime and control interface, but
it is not API-stable. Breaking changes may still be made when needed to correct
behavior or simplify the architecture.

## 0.1.1-alpha.1 - 2026-09-21

- Added magnetic endless-canvas window snapping with configured gaps.
- Added directional arrow-key focus navigation.
- Added runtime cursor theme control and camera state reporting to `zwwmctl`.
- Improved zoom-consistent shader rounding and cursor handling.

## 0.1.0-alpha.1 - 2026-09-20

Basically first public alpha release.

### Highlights

- Independent C++23 zwayland client/server runtime and typed protocol scanner.
- Direct DRM/KMS and nested Wayland backends with Xwayland support.
- Master-stack, focus-fibonacci, and endless-canvas layouts.
- Native and Xwayland window management, tags, focus, clipboard, and drag-and-drop.
- Layer shell, session lock, pointer constraints, relative pointer, and idle notify.
- OpenGL animations, backdrop blur, glass, and atomically reloadable user shaders.
- Screencopy and compositor-supervised XDG desktop portal integration.
- Wayland-native `zwwmctl` status, dispatch, event, reload, and shader controls.

### Stability

This release is an alpha. Configuration fields, private zwwm protocols, and
internal APIs may change before a stable release. Wayland client compatibility
remains a core project goal.
