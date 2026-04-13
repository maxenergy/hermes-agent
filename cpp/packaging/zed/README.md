# Hermes Agent — Zed integration

Zed speaks the Agent Client Protocol (ACP) natively, so hooking Hermes in
is a one-file config change.

1. Make sure `hermes` is on your `$PATH`, or use an absolute path in `command`.
2. Copy the relevant block from `hermes.toml` into your Zed agent config
   (refer to the Zed docs for the exact location — typically
   `~/.config/zed/settings.json` under `agent_servers` / `agents`).
3. Restart Zed. `ANTHROPIC_API_KEY` (or another supported env var) is picked
   up automatically by the Hermes ACP adapter.

## JSON equivalent

If your Zed install wants JSON instead of TOML:

```json
{
  "agents": {
    "hermes": {
      "command": "hermes",
      "args": ["acp"],
      "env": {
        "ANTHROPIC_API_KEY": "${env:ANTHROPIC_API_KEY}"
      }
    }
  }
}
```

## Protocol version

The adapter advertises ACP protocol v1 (see
`cpp/acp/src/acp_adapter.cpp :: capabilities`). If Zed bumps the minimum
protocol version, update `capabilities()` to match.
