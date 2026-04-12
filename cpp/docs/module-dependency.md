# Module Dependency Listing

Each row lists a CMake library target under `cpp/` and its direct
first-party dependencies (other `hermes::*` targets). Third-party
dependencies (OpenSSL, libcurl, nlohmann_json, yaml-cpp, SQLite, re2,
Boost, CLI11, inja, cmark-gfm, md4c, utfcpp, spdlog, jwt-cpp, FTXUI,
cpr, zlib) are omitted for brevity — see each subdirectory's
`CMakeLists.txt` for the authoritative list.

| Target | Depends on (first-party) |
|--------|--------------------------|
| `hermes::core` | (none — foundation) |
| `hermes::approval` | `hermes::core` |
| `hermes::config` | `hermes::core` |
| `hermes::profile` | `hermes::core` |
| `hermes::auth` | `hermes::core` |
| `hermes::state` | `hermes::core` |
| `hermes::external` | `hermes::core` (interface-only header pack) |
| `hermes::llm` | `hermes::core` |
| `hermes::environments` | `hermes::core`, `hermes::llm` |
| `hermes::agent` | `hermes::core`, `hermes::config`, `hermes::state`, `hermes::llm` |
| `hermes::tools` | `hermes::core`, `hermes::llm`, `hermes::environments`, `hermes::state` |
| `hermes::skills` | `hermes::core` |
| `hermes::cron` | `hermes::core` |
| `hermes::gateway` | `hermes::core`, `hermes::config`, `hermes::llm`, `hermes::agent` |
| `hermes::cli` | `hermes::core`, `hermes::config`, `hermes::tools`, `hermes::skills`, `hermes::cron`, `hermes::profile`, `hermes::gateway` |
| `hermes::batch` | `hermes::agent`, `hermes::llm` |
| `hermes::plugins` | `hermes::core`, `hermes::tools` |
| `hermes::mcp_server` | `hermes::state`, `hermes::core` |
| `hermes::acp` | `hermes::core` |

## Top-level binaries

| Binary | Entrypoint | Links |
|--------|-----------|-------|
| `hermes_cpp` | `cli/main.cpp` | `hermes::cli` (pulls all transitive deps) |
| Gateway runner | built into `hermes::gateway` + `hermes::cli` subcommand | n/a |
| MCP server | built into `hermes::mcp_server` + `hermes::cli` subcommand | n/a |

## Test targets

Each library has a sibling `tests/` directory producing a `<name>_tests`
executable registered via `gtest_discover_tests`. All test targets link
against `GTest::gtest_main` and their respective library.
