# Contributing

zwwm is an early-stage C++23 compositor. Focused bug reports and small,
well-scoped changes are preferred.

## Development Environment

Enter the pinned Nix development shell:

```sh
nix develop
```

Build a release configuration with CMake:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DXWAYLAND_ENABLE=ON
cmake --build build -j2
```

The equivalent Meson build is:

```sh
meson setup build-meson --buildtype=release --prefix=/usr -Dxwayland=true
meson compile -C build-meson -j2
```

## Workflow

Changes move through three stages:

```text
your branch -> dev -> main
```

1. Create a branch for your change, named after what it does, such as
   `fix-something`.
2. Open a pull request against `dev`.
3. Changes reach `main` from `dev`. Do not open pull requests against `main`.

Commit naming is not critical. Use whatever describes your change.

## Before Opening a Pull Request

- Both the CMake and Meson builds pass.
- The change has been tested in a real zwwm session.

## Changes

- Keep changes direct and narrowly scoped; fix root causes rather than adding wrappers.
- Keep both CMake and Meson production builds working.
- Do not add generated build output or machine-specific configuration.
- Document user-visible configuration and protocol changes.
- Large changes require large evidence to be accepted.

For rendering changes, failures in optional blur or glass preparation must fall
back safely and must not reject an otherwise presentable frame.

## AI-Assisted Contributions

AI tools may assist your work. You are responsible for everything you submit, so
you must understand the code and have tested it yourself.

- PR are human authored only.
- Pull requests generated automatically by an LLM(Like Claude) are straightup declined without discussion.
