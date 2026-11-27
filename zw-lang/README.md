# zw-lang

Parser and validator for zwwm's `.zw` config. Link `zwwm::lang`; component
rules are passed in, not built in.

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

Values: strings, booleans, integers, decimals, `null`, arrays, comma lists,
objects.

### Variables

UPPERCASE names are variables; `$NAME` falls back to the environment. The last
definition wins, even across imports. Strings expand `$NAME` and `${NAME}`;
unknown names stay literal so shell `$1` still works. `\$` is a literal dollar.

### Decimals

`text-rounding = 0.2` means `text-rounding-per-mille = 200`. Up to 3 places,
setting values only.

### Strings

Escapes: `\n \t \r \e \" \\ \$ \u{2192}`. Other backslashes stay literal.

### Diagnostics

Errors carry file, line and column; parsing resumes at the next field. Repeated
fields warn and the last one wins.

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

Use the lookups and `as_*` accessors rather than the variant directly.
Variables are already resolved. zwwm's rules live in
`zwwm/src/config_validation.cpp`.
