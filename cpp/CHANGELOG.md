# Changelog

## [0.2.0] - 2026-04-14

### Added
- **Qwen Code OAuth provider**: device-code + PKCE S256; shares
  `~/.qwen/oauth_creds.json` with qwen-code CLI. `QwenClient` synthesises
  all 9 portal.qwen.ai fingerprint fields (UA / coder-model alias /
  content blocks / placeholder tools / metadata / vl_high_resolution_images /
  system msg / HTTP/1.1 / stream=true).
- **MCP client completion**: auto-reconnect with jittered exp backoff,
  `sampling/createMessage` routed through LlmClient (gated by
  `allow_sampling` + optional approver), `notifications/tools/list_changed`
  dynamic rediscovery, OAuth initiator on 401.
- **Discord + Slack WebSocket gateway drivers** via Boost.Beast —
  Discord v10 (Hello/Identify/Heartbeat/Resume/Dispatch), Slack Socket
  Mode + legacy RTM.
- **Gateway polish**: media cache (content-addressed, 429/5xx retry,
  SSRF guard), exponential-backoff reconnect with fatal/retryable
  classification, Telegram `setMyCommands`, Slack `/hermes` subcommand
  router + thread-aware reply gating, per-platform channel cache, scoped
  locks on WhatsApp / Signal / Matrix.
- **WhatsApp pairing** (phone+code), disappearing messages, group v2
  (LID + legacy JID). **Signal** expiration, safety-number verify, group
  normalisation.
- **Voice subsystem**: whisper.cpp CLI integration, `voice_mode`
  streaming STT→LLM→TTS pipeline with VAD segment boundaries.
- **Environments**: `spawn_via_env()` remote forwarding, Docker image
  digest pinning + anon-volume cleanup, `DotfileManager`, OSC 7 CWD
  parser, `ScpCopier`, real POSIX `spawn_local()` with timeout/kill.
- **LLM infrastructure**: `CredentialPool` (TTL + refresher),
  `resolve_runtime_provider()` (8 providers), `codex_models`,
  `model_switch` hot-swap with tokenizer invalidation + tier-down.
- **Web search**: Tavily, Parallel.ai, Brave, Google CSE backends + TTL
  cache. **Image gen**: Flux (BFL + Replicate fallback), Ideogram v2/v3,
  generic Replicate, `list_image_models` cache.
- **RL + batch**: HF SFT schema, SWE-task runner, env-aware batch
  dispatch, `hermes rl train|eval|list-environments` CLI, 30-min
  checkpoint watchdog.
- **ACP auth** (api-key + oauth, per-session token map),
  VS Code / Zed / JetBrains editor integration scaffolds,
  `FtxuiEditor` + `curses_ui` (no `\033[K`).
- **Agent layer**: `context_references`, `compression_feedback`,
  `async_bridge`, `insights` module.
- **Config**: v1→v6 per-version migration, `--profile/-p` argv
  pre-parse, dynamic plugin `rebuild_lookups()` + derived
  `gateway_known_commands()`.
- **CLI long-tail**: `hermes auth` multi-provider, `hermes webhook
  {install,list,remove}`, `hermes dump {sessions,config}` with
  `--since`/`--output`/redaction, `hermes providers {list,test}`,
  `hermes runtime {list,select}`.
- **Hooks + process introspection**: YAML `hook_discovery` with
  subprocess exec, `looks_like_gateway_process` +
  `get_process_start_time()` cross-platform.
- **Infra**: UDS JSON-RPC 2.0 server/client, `/update` prompt (24h
  throttle + CI/TTY skip), ffmpeg subprocess wrapper.
- **Skills**: 78 builtin SKILL.md copied from Python tree, 3-level
  discovery. **CI**: fast-gate + full cross-platform matrix + vcpkg
  binary cache. **Packaging**: Termux, WSL2 README, multi-arch Docker
  smoke test.
- **Joint integration tests**: 23 cross-module tests.
- **8 missing tool modules**: `checkpoint_manager`, `mcp_oauth`,
  `tirith_security`, `skills_sync`, `browser_camofox`, neutts TTS,
  `tool_backend_helpers`, `debug_helpers`.

### Changed
- Project version 0.1.0 → 0.2.0.
- Spinner replaces `\033[K` with `last_visible_len_`-tracked space
  padding (Python prompt_toolkit invariant).
- libcurl pinned to HTTP/1.1 (HTTP/2 aborts on portal.qwen.ai).
- `McpStdioTransport` uses recursive mutex + persistent read buffer +
  SIGPIPE ignore.
- `ProcessRegistry::kill()` + email IMAP IDLE now work on Windows
  (Winsock2, OpenProcess/TerminateProcess).

### Metrics
- 1381/1381 tests passing (up from 887).
- Plan: only WSL2 host-matrix run + release cadence remain unchecked
  (both non-code).

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
