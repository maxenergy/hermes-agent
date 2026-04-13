# Hermes Agent — JetBrains plugin (scaffolding)

This directory contains:

- `plugin.xml` — IntelliJ platform plugin descriptor
- `src/main/kotlin/com/hermes/agent/HermesAgentService.kt` — application
  service that spawns `hermes acp` and speaks ACP over stdio

## Status

Scaffolding only. **Not tested in an IDE.** To build a real plugin:

1. Add `build.gradle.kts` with the
   [IntelliJ Platform Gradle Plugin](https://plugins.jetbrains.com/docs/intellij/welcome.html).
2. Replace the hand-rolled JSON emitter with `kotlinx.serialization`.
3. Add `HermesToolWindowFactory` + `StartAction` / `StopAction` classes
   referenced in `plugin.xml`.
4. Wire notifications from the reader thread through the `Hermes`
   notification group.
5. Pack with `./gradlew buildPlugin`.

## Why so minimal

`hermes acp` is the source of truth — the plugin is only a thin transport
shim. All auth / session / tool flows live in the C++ `AcpAdapter`.
