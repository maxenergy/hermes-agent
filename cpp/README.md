# hermes_cpp — C++17 Backend Port

Phase 0 scaffolding for the C++17 reimplementation of the Hermes Agent
backend. This subtree lives alongside the existing Python codebase and
builds independently.

## Prerequisites

- CMake >= 3.20
- A C++17 compiler (GCC 11+, Clang 13+, or AppleClang 14+)
- Internet access the first time you configure (GoogleTest is vendored
  via `FetchContent`)

Optional for future phases:

- [vcpkg](https://github.com/microsoft/vcpkg) — `export VCPKG_ROOT=/path/to/vcpkg`
  before the first `cmake --preset` and the top-level `CMakeLists.txt`
  will pick the toolchain up automatically.

## Build & test

```bash
cd cpp
cmake --preset default
cmake --build build/default -j
ctest --test-dir build/default --output-on-failure
```

Or, equivalently, using the matching build/test presets:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

## Available presets

| Preset    | Build type       | Notes                                           |
| --------- | ---------------- | ----------------------------------------------- |
| `default` | `Debug`          | Everyday development. Tests enabled.            |
| `release` | `RelWithDebInfo` | Optimised with debug symbols.                   |
| `asan`    | `Debug`          | Adds `-fsanitize=address,undefined`.            |

Binary directories land under `cpp/build/<preset>/`.

## Layout

```
cpp/
  CMakeLists.txt        — top-level project + warning flags
  CMakePresets.json     — default / release / asan
  vcpkg.json            — dependency manifest (Phase 1+ wiring)
  external/             — find_package stubs for vcpkg packages
  core/                 — hermes_core static library
    include/hermes/core/
    src/
    tests/              — GoogleTest suite (vendored via FetchContent)
```

The Phase 0 `hermes_core` library intentionally depends only on the
C++17 standard library, so the build works on a clean Linux box with no
vcpkg install. Phase 1+ will wire HTTP, SQLite, YAML, and JSON support
through the dependencies declared in `vcpkg.json`.

## Dependency manifest note

`vcpkg.json` lists `md4c` as the CommonMark parser because
`cmark-gfm` was not available in the vcpkg baseline at the time of
scaffolding. If a future baseline adds `cmark-gfm`, we can swap it back.
