# Changelog

## [0.1.0] - 2026-04-13

### Added
- Batch 12: Modal, Daytona, Singularity, claw, Honcho, IMAP platform/tool integrations
- Batch 11: PTY support, Ollama client, smart routing, model normalization, CI wiring, Doxygen docs
- Batch 13: architecture docs (Mermaid diagrams), module dependency listing, release + build-release scripts
- Install hook copying `skills/` and `optional-skills/` to `share/hermes/`
- `HERMES_SKILLS_SEARCH_PATH` env var + system-path fallback in `skill_utils::get_all_skills_dirs()`
- `cpp/docs/architecture.md` (6 Mermaid diagrams: component, agent loop, tool dispatch, gateway, memory/context, build deps)
- `cpp/docs/module-dependency.md` (first-party link graph for all 20 library targets)
- `cpp/scripts/release.sh` semver bumper + tagger; `cpp/scripts/build-release.sh` release artifact builder

### Changed
- `cpp/CMakeLists.txt` project version bumped 0.0.1 -> 0.1.0

- Windows ConPTY + `TerminateProcess` + `GetProcessTimes` under `#ifdef _WIN32`
- Multi-arch Dockerfile (`ARG TARGETARCH`) + `docker-buildx.sh` + `termux-build.sh`
- GitHub Copilot OAuth device code flow (`CopilotOAuth` with polling + token exchange)
- Nous Research subscription status check (`SubscriptionStatus` with 1h cache)
- CLI long-tail: `hermes model switch`, `providers list`, `memory setup`,
  `dump <sessions|config|memory>`, `webhook install`, `runtime terminal`
- CI jobs: `build-arm64`, `build-windows` (vcpkg), `build-wsl2`,
  `docker-multiarch` (amd64 + arm64)
- Discord voice via libopus: `OpusCodec` (48kHz stereo 20ms VOIP)
  + `DiscordAdapter::join_voice/send_voice_pcm/process_voice_rtp`
  + SSRC↔user_id mapping (system has libopus-dev → enabled)
- Matrix E2EE via libolm: `OlmAccount`, `OlmSession`,
  `MegolmOutboundSession`, `MegolmInboundSession` + adapter wiring
  (compile-time opt-in via `find_path(olm/olm.h)`)

### Metrics
- 887/887 tests passing (up from 728 in 0.0.1-alpha)

## [0.0.1-alpha] - 2026-04-12

### Added
- Core utility library (12 modules): strings, path, atomic_io, env, time, redact, logging, retry, url_safety, ansi_strip, fuzzy, patch_parser
- Configuration system with YAML merge, profile isolation, credential management
- SessionDB (SQLite FTS5 + WAL), ProcessRegistry, MemoryStore, TrajectoryWriter
- LLM clients: OpenAI, Anthropic, OpenRouter with SSE streaming + prompt caching
- AIAgent conversation loop with context compression + memory management
- Tool registry with 49 tool implementations
- Danger-command scanner (45+ patterns) + approval system
- Terminal environments: Local (fork+exec+PTY), Docker, SSH
- Skills system: discovery, loading, slash-command injection, Skills Hub
- Cron scheduler with 5-field parser + delivery router
- Gateway: 18 platform adapters with real HTTP, session store, hooks, pairing
- CLI: 36 commands, 4 skins, interactive REPL
- Batch trajectory runner + compressor
- MCP server (10 tools, stdio JSON-RPC) + MCP client (stdio transport)
- CDP browser backend (Chrome DevTools Protocol)
- ACP editor adapter
- Plugin system (dlopen)
- 728 tests, zero stubs
