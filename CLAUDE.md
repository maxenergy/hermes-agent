# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> A more exhaustive contributor guide lives in `AGENTS.md` — read it for skin engine details, profile internals, and the complete list of pitfalls. This file is the fast on-ramp.

## Environment

```bash
source venv/bin/activate   # ALWAYS activate before running Python or pytest
```

Setup from scratch:
```bash
uv venv venv --python 3.11
source venv/bin/activate
uv pip install -e ".[all,dev]"
```

## Tests

```bash
python -m pytest tests/ -q                       # full suite (~3000 tests, ~3 min)
python -m pytest tests/test_model_tools.py -q    # single file
python -m pytest tests/gateway/ -q               # subtree
python -m pytest tests/test_cli_init.py::TestName::test_case -q   # single test
```

Run the full suite before pushing.

## Run / Entry Points

```bash
hermes              # interactive CLI (cli.py → HermesCLI)
hermes gateway      # messaging gateway (gateway/run.py)
hermes setup        # interactive setup wizard
hermes doctor       # diagnose config / env issues
python run_agent.py # AIAgent CLI directly (no TUI)
```

User config lives in `~/.hermes/config.yaml` and `~/.hermes/.env` (or `~/.hermes/profiles/<name>/...` under a profile).

## High-Level Architecture

The project is a synchronous Python agent loop with a tool registry, plus a separate async messaging gateway. The two share session storage, slash commands, and the tool layer.

```
tools/registry.py            # central registry — schemas, handlers, dispatch (no deps)
        ↑
tools/*.py                   # each file calls registry.register() at import time
        ↑
model_tools.py               # _discover_tools() imports all tools, get_tool_definitions()
        ↑
run_agent.py (AIAgent)       # the conversation loop — used by:
   ├── cli.py (HermesCLI)            # interactive TUI
   ├── gateway/run.py                # Telegram/Discord/Slack/WhatsApp/Signal/Matrix
   ├── batch_runner.py               # parallel batch trajectories
   ├── acp_adapter/                  # ACP server (VS Code / Zed / JetBrains)
   └── environments/                 # Atropos RL training envs
```

**`AIAgent.run_conversation()` in `run_agent.py`** is the core loop. It is fully synchronous: call the model, dispatch any tool_calls through `handle_function_call()` in `model_tools.py`, append results, repeat until no tool calls or `max_iterations` is hit. Messages use the OpenAI format; reasoning content is stored on `assistant_msg["reasoning"]`. Agent-level tools (todo, memory) are intercepted in `run_agent.py` *before* `handle_function_call()` — see `tools/todo_tool.py` for the pattern.

**Slash commands are defined once** in `COMMAND_REGISTRY` (`hermes_cli/commands.py`) as `CommandDef` objects. Every consumer is derived: CLI dispatch (`cli.py:HermesCLI.process_command`), gateway dispatch (`gateway/run.py`), Telegram BotCommand menu, Slack `/hermes` subcommand routing, autocomplete, and `/help` output. **To add a command**: add a `CommandDef` to the registry, then add a handler branch in both `cli.py` and (if gateway-visible) `gateway/run.py`. Adding an alias requires only the `aliases` tuple.

**Adding a tool** requires three edits: (1) create `tools/your_tool.py` and call `registry.register(...)` at module import; (2) add the import to `_discover_tools()` in `model_tools.py`; (3) add the tool name to `_HERMES_CORE_TOOLS` in `toolsets.py` or to a new toolset. All handlers MUST return a JSON string. Use `display_hermes_home()` in schema descriptions and `get_hermes_home()` for state file paths — never hardcode `~/.hermes`.

**Sessions and history**: `hermes_state.py` (`SessionDB`) is a SQLite store with FTS5 search; the gateway has its own `gateway/session.py:SessionStore` for cross-platform persistence.

## Profiles (Multi-Instance)

Hermes supports isolated profiles via `HERMES_HOME`. `_apply_profile_override()` in `hermes_cli/main.py` sets the env var **before any module imports**, so all 119+ `get_hermes_home()` callers automatically scope to the active profile.

Rules for profile-safe code:
- **State paths**: `from hermes_constants import get_hermes_home` — never `Path.home() / ".hermes"`.
- **User-facing messages**: `display_hermes_home()` — returns `~/.hermes` or `~/.hermes/profiles/<name>`.
- **Profile *operations*** (the profile manager itself) are HOME-anchored, not HERMES_HOME-anchored — `_get_profiles_root()` returns `Path.home() / ".hermes" / "profiles"` so `hermes -p coder profile list` sees all profiles regardless of which is active.
- **Gateway adapters** that connect with a unique credential should call `acquire_scoped_lock()` / `release_scoped_lock()` from `gateway.status` — see `gateway/platforms/telegram.py` for the canonical pattern.
- **Tests** must not write to `~/.hermes/`. The `_isolate_hermes_home` autouse fixture in `tests/conftest.py` redirects `HERMES_HOME` to a temp dir; profile-related tests must additionally `monkeypatch.setattr(Path, "home", ...)` — see `tests/hermes_cli/test_profiles.py`.

## Prompt Caching is Load-Bearing

Hermes relies on Anthropic prompt caching across the whole conversation. **Do NOT**:
- Alter past context mid-conversation
- Change the toolset list mid-conversation
- Reload memories or rebuild the system prompt mid-conversation

The only sanctioned mid-conversation context mutation is context compression (`agent/context_compressor.py`). Skill slash commands inject the skill body as a **user message**, not into the system prompt, for the same reason (`agent/skill_commands.py`).

## Working Directory Convention

- **CLI** uses the current directory (`os.getcwd()`).
- **Messaging gateway** uses `MESSAGING_CWD` (defaults to the user's home).

## Known Pitfalls

- **Don't hardcode `~/.hermes`** in code or display strings — use `get_hermes_home()` / `display_hermes_home()`. Hardcoding broke profiles in 5 places (PR #3575).
- **Don't use `simple_term_menu`** for interactive menus — it ghosts on scroll under tmux/iTerm2. Use `curses` (stdlib) — see `hermes_cli/tools_config.py`.
- **Don't emit `\033[K`** (ANSI erase-to-EOL) in spinner/display code — leaks as literal `?[K` under `prompt_toolkit`'s `patch_stdout`. Pad with spaces instead: `f"\r{line}{' ' * pad}"`.
- **Don't reference other tools by name in tool schema descriptions** — that tool may be disabled or missing an API key, leading the model to hallucinate calls. If a cross-reference is needed, inject it dynamically in `get_tool_definitions()` in `model_tools.py` (see the `browser_navigate` / `execute_code` post-processing).
- **`_last_resolved_tool_names` in `model_tools.py` is a process global**, temporarily restored by `delegate_tool.py:_run_single_child()` around subagent runs — assume it can be stale during child agent execution.

## Config Surface

| Loader | Used by | Where |
|---|---|---|
| `load_cli_config()` | CLI mode | `cli.py` |
| `load_config()` | `hermes tools`, `hermes setup` | `hermes_cli/config.py` |
| Direct YAML load | Gateway | `gateway/run.py` |

To add a config option: edit `DEFAULT_CONFIG` in `hermes_cli/config.py` and bump `_config_version` to trigger migration. To add an env var: add to `OPTIONAL_ENV_VARS` in the same file with `description`/`prompt`/`url`/`password`/`category`.
