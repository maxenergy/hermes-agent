# Hermes Agent — C++17 Backend

C++17 reimplementation of the [hermes-agent](https://github.com/NousResearch/hermes-agent) backend.

## Build

```bash
cd cpp
cmake --preset default    # Debug
cmake --preset release    # Release
cmake --preset asan       # Address Sanitizer
cmake --build build/<preset> -j
ctest --test-dir build/<preset>
```

## Dependencies

Core library (hermes_core) has zero external dependencies beyond C++17 stdlib.
Full build uses: nlohmann-json, yaml-cpp, SQLiteCpp, OpenSSL, spdlog (optional via vcpkg).

## Architecture

The backend is organized as 18 static libraries with clear dependency boundaries.
The AIAgent conversation loop drives the core flow: PromptBuilder constructs
messages, the LLM client calls the provider API (via injectable HttpTransport),
tool calls are dispatched through the ToolRegistry, and ContextCompressor
manages context window limits. The Gateway layer adapts this pipeline to 18
platform transports (Telegram, Discord, Slack, etc.), while the CLI provides
direct terminal access.

## Modules

| Library | Purpose |
|---------|---------|
| hermes_core | Strings, paths, atomic I/O, env, time, redact, retry, URL safety |
| hermes_config | Config loading, profile management, credentials |
| hermes_state | SessionDB (SQLite+FTS5), ProcessRegistry, MemoryStore |
| hermes_llm | LLM clients (OpenAI/Anthropic/OpenRouter), prompt caching |
| hermes_agent | AIAgent conversation loop, PromptBuilder, ContextCompressor |
| hermes_tools | Tool registry + 49 tool implementations |
| hermes_approval | Dangerous-command scanner, approval state, website policy |
| hermes_environments | Terminal backends (Local/Docker/SSH) |
| hermes_skills | Skill discovery, loading, slash-command injection |
| hermes_cron | Cron scheduler, job store, delivery router |
| hermes_gateway | Gateway runner, session store, hooks, pairing, 18 platform adapters |
| hermes_cli | CLI commands, skin engine, hermes entry point |
| hermes_batch | Batch trajectory runner + compressor |
| hermes_mcp_server | Hermes as MCP server (stdio JSON-RPC) |
| hermes_acp | Editor integration (ACP protocol) |
| hermes_plugins | Plugin system (dlopen) |

## Available Presets

| Preset | Build type | Notes |
|--------|-----------|-------|
| `default` | `Debug` | Everyday development. Tests enabled. |
| `release` | `RelWithDebInfo` | Optimised with debug symbols. |
| `asan` | `Debug` | Adds `-fsanitize=address,undefined`. |

## Testing

624+ tests across unit, integration, and benchmark suites. Run with:

```bash
ctest --test-dir build/default
```

Integration tests and benchmarks are in `cpp/tests/`. Individual suites:

```bash
# Integration tests only
./build/default/tests/hermes_integration_tests

# Benchmarks only
./build/default/tests/hermes_benchmarks
```

## Layout

```
cpp/
  CMakeLists.txt        — top-level project + warning flags
  CMakePresets.json     — default / release / asan
  vcpkg.json            — dependency manifest
  external/             — find_package stubs for vcpkg packages
  core/                 — hermes_core static library
  agent/                — AIAgent, PromptBuilder, ContextCompressor
  tools/                — ToolRegistry + 49 tool implementations
  llm/                  — LLM client abstraction + providers
  state/                — SessionDB, ProcessRegistry, MemoryStore
  config/               — Config loader, env expansion
  approval/             — Command scanner, approval state
  environments/         — Terminal backends
  skills/               — Skill discovery + loading
  cron/                 — Cron scheduler + job persistence
  gateway/              — Gateway runner + 18 platform adapters
  cli/                  — CLI commands + skin engine
  batch/                — Batch trajectory runner
  mcp_server/           — MCP server (stdio JSON-RPC)
  acp/                  — ACP editor integration
  tests/                — Integration tests + benchmarks
```
