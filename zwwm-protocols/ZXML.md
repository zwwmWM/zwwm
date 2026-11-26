# zxml

zxml is a C++-flavoured source format for Wayland protocols. It describes the
same model as Wayland XML, and `zwayland-scanner` reads both. Project protocols
in `protocols/` are written in zxml; the vendored upstream snapshots stay XML.

```sh
zwayland-scanner zwayland-server-header foo.zxml foo-server.h
zwayland-scanner xml  foo.zxml foo.xml     # Wayland XML for other generators
zwayland-scanner zxml foo.xml  foo.zxml    # convert existing XML
```

## Example

```cpp
/// dismissible keyboard focus
protocol zwwm_focus_grab_v1;

copyright R"(
Copyright 2026 binarylinuxx. SPDX-License-Identifier: BSD-3-Clause
)";

/// grab keyboard focus for a layer panel
///
/// Clicking or touching outside dismisses the grab.
interface zwwm_focus_grab_manager_v1 version 1 {
  enum error {
    invalid_surface = 0,  ///< the surface has no layer role
  }

  request ~destroy();
  request get_grab(new<zwwm_focus_grab_v1> id, object<wl_surface> surface);
}

interface zwwm_focus_grab_v1 version 2 {
  [[bitfield]] enum reason {
    outside = 1,
    locked [[since(2)]] = 2,
  }

  request ~destroy();
  [[since(2)]] request release();

  event dismissed(uint<reason> reason);
}
```

## Syntax

| zxml | Wayland XML |
| --- | --- |
| `protocol name;` | `<protocol name>` (must come first) |
| `copyright "..." "...";` or `R"( ... )"` | `<copyright>`; adjacent strings concatenate |
| `interface name version N { ... }` | `<interface name version>` |
| `[[frozen]] interface ...` | `frozen="true"` |
| `enum name { a = 0, b = 0x2, }` | `<enum>` / `<entry>` |
| `[[bitfield]] enum ...` | `bitfield="true"` |
| `entry [[since(2)]] = 1` | entry `since` |
| `request name(args);` / `event name(args);` | `<request>` / `<event>` |
| `request ~name();` | `type="destructor"` |
| `[[since(3), deprecated(5)]] request ...` | `since`, `deprecated-since` |

Argument types:

| zxml | XML type |
| --- | --- |
| `int`, `uint`, `fixed`, `string`, `array`, `fd` | same name |
| `int<enum>`, `uint<iface.enum>` | `enum="..."` |
| `object`, `object<wl_surface>` | `object`, `interface="wl_surface"` |
| `new<wl_callback>` | `new_id` with `interface` |
| `new` | generic `new_id` (interface name and version on the wire) |
| `T?` | `allow-null="true"` (string and object only) |

Enum entry names may start with a digit (`90 = 1`) and values may be negative
or hexadecimal. Trailing commas are allowed in enums and argument lists.

## Documentation

`///` comments document the declaration that follows. The first paragraph is
the summary; everything after the first blank `///` line is the description
body. A body with no summary starts with a blank `///` line.

`///<` gives a one-line summary to the preceding argument, entry or message,
before or after its comma. `//` and `/* */` are ordinary comments and are
dropped.

Keywords are contextual, so arguments may be named `interface`, `version`,
`object` and so on.
