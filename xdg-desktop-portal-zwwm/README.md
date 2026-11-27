# xdg-desktop-portal-zwwm

Portal backend for zwwm: Screenshot, ScreenCast, Access, FileChooser and
Settings. GTK handles the rest.

zwwm spawns it directly (no D-Bus activation) and authenticates it through an
inherited socket; capture rights end when the process or connection goes away.

## Screenshot

- Full-output, window or region capture, interactive or not
- PNG, BMP, TGA, JPEG and PPM output

Public screencopy for tools like `grim` is separate from the portal.

## ScreenCast

One monitor or window per session, BGRx up to 60 fps, DMA-BUF or MemFd.
Window streams contain only the client's surfaces, without borders or effects.
The cursor can be hidden or embedded.

Not implemented: metadata cursor, multiple sources, virtual outputs,
persistence, resize.

## Settings

Only `org.freedesktop.appearance` `color-scheme`, fixed to dark.

## Dependencies

Qt 6 (Core, DBus, Gui, Widgets), `libzwayland-client`, PipeWire. Images are
written with the vendored `stb_image_write.h` (see `vendor/README.md`).
