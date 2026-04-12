# Changelog

## [0.0.1] - 2026-04-12

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
