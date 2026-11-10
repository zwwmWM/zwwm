# zw-lang

`zw-lang` is the standalone parser and validation library for zwwm
configuration. Consumers link `zwwm::lang`; compositor-specific validation is
provided through callbacks rather than compiled into the language module.

## Syntax

```zw
-- Variables and references
SUPER = "Super"
TERMINAL = "ghostty"

bind = $SUPER, Return, exec, $TERMINAL
items = ["one", "two"]
output = {
  name = "DP-1"
  mode = "2560x1440@144"
}
```

The language supports strings, booleans, integers, decimals, `null`, variable
references, arrays, comma-separated lists, objects, comments, and trailing array
commas.

### Variables

UPPERCASE assignments are variables. `$NAME` uses one, falling back to the
environment. The last definition wins everywhere, so a variable set after an
`import` also changes values in the imported file. Strings splice variables with
`"$NAME"` or `"${NAME}"`. Names that are neither config nor environment
variables stay literal, so shell commands keep `$1` or `$(...)`; write `\$` for a
literal dollar.

```zw
TERMINAL = "foot"
bind = SUPER, Return, exec, "$TERMINAL -e htop"
```

### Decimals

A decimal with up to 3 places is shorthand for the `-per-mille` setting of the
same name: `text-rounding = 0.2` is read as `text-rounding-per-mille = 200`.
Decimals are only allowed as setting values, not as array or list items.

### Strings

Escapes are `\n`, `\t`, `\r`, `\e`, `\"`, `\\`, `\$`, and `\u{2192}`. Other
backslashes stay literal, so regexes such as `"^foo\.bar$"` keep their
backslash.

### Diagnostics

Every value carries its file, line, and column, including values from imported
files. A broken field reports one error and parsing resumes at the next field.
A field repeated in one object is a warning; the last one is used.

## API

```cpp
const auto parsed = zwwm::lang::parse_config_file(path);
if (!parsed.ok()) {
  // Inspect parsed.diagnostics.
}

const auto checked = zwwm::lang::validate_config(parsed.config, rules);
const auto* value = zwwm::lang::find_assignment(parsed.config, "output");
const auto* object = value == nullptr ? nullptr : zwwm::lang::as_object(*value);
```

Use `find_assignment`, `find_field`, and the `as_*` accessors instead of
depending on the internal variant representation; both lookups return the last
occurrence. Parsing resolves variables, so consumers see plain values.
`validate_config` only checks language rules; components pass their own
`ValidationRule`s through `ValidationContext` (zwwm's live in
`zwwm/src/config_validation.cpp`).
