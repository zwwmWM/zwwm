#!/usr/bin/env python3

import pathlib
import sys

# Output name, then one source per embedded constant in this order.
NAMES = ("zwwm_library", "glass_physics", "surface_vertex", "surface_fragment", "glass_fragment",
         "kawase_fragment")


def main() -> int:
    if len(sys.argv) != len(NAMES) + 2:
        raise SystemExit("usage: embed-shaders.py OUTPUT ZWWM_GLSL GLASS_PHYSICS SURFACE_VERT SURFACE_FRAG "
                         "GLASS_FRAG KAWASE_FRAG")

    output = pathlib.Path(sys.argv[1])
    lines = ["#pragma once", "", "namespace zwwm::renderer::shader {", ""]
    for name, path in zip(NAMES, sys.argv[2:]):
        # #version must stay on the first line of each stage source.
        source = pathlib.Path(path).read_text()
        lines.extend((f'inline constexpr char {name}[] = R"zwwm({source}', ')zwwm";', ""))
    lines.append("}  // namespace zwwm::renderer::shader")
    output.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
