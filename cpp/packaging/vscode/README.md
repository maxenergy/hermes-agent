# hermes-agent VS Code extension

A VS Code extension that spawns `hermes acp` and speaks the Agent Client
Protocol (ACP) over stdio.

## Build

```bash
cd cpp/packaging/vscode
npm install
npx tsc -p .
```

The compiled output lands in `out/extension.js`.

## Dev

```bash
# Open this folder in VS Code and press F5 to launch an extension host.
```

## Config

Settings (in VS Code `settings.json`):

| key | default | description |
|---|---|---|
| `hermes.executable` | `hermes` | Path to the `hermes` binary |
| `hermes.acpArgs` | `["acp"]` | Args passed to the binary |
| `hermes.authMethod` | `api-key` | `api-key` or `oauth` |
| `hermes.apiKeyEnvVar` | `ANTHROPIC_API_KEY` | Env var that supplies the API key |

## Status

Scaffolding only — not yet published to the marketplace. The ACP client
handles `initialize` and `authenticate`; prompt/session flows follow the
same pattern (`transport.call("new_session", ...)` etc.).
