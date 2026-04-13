// Hook discovery + manifest parsing + subprocess execution.
//
// Mirrors hermes_cli/hooks.py + the subset of gateway/hooks.py that scans
// `${HERMES_HOME}/hooks/*` directories for `HOOK.yaml` manifests describing
// shell or Python entry points.  The primary surface is `discover_hooks()`
// which returns the parsed manifests, and `execute_hook()` which spawns the
// subprocess, pipes the event payload via stdin, and decodes a JSON
// response of the form `{ "action": "continue" | "block", "message": "..." }`.
//
// Everything is synchronous and dependency-light: yaml-cpp + nlohmann::json
// (already wired through hermes::config) and POSIX `fork/execvp` (or
// `_spawnvp` on Windows -- see TODO in execute_hook implementation).
//
// The class is intentionally process-spawn based -- we cannot dlopen
// arbitrary user `.py` or `.sh` files, so each hook fires as its own child
// process, matching the safer of the two Python reference implementations.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::cli {

// Supported lifecycle events.  The "match" field on a manifest is only
// consulted for "pre-tool" / "post-tool"; for the other events any hook
// registered for the event fires unconditionally.
enum class HookEvent {
    PreTool,
    PostTool,
    SessionStart,
    SessionEnd,
    UserPrompt,
    Unknown,
};

HookEvent parse_event(const std::string& s);
std::string event_to_string(HookEvent e);

// Decoded HOOK.yaml manifest plus the directory it lives in.
struct HookManifest {
    std::string name;
    HookEvent event = HookEvent::Unknown;
    std::string event_raw;            // Original string from YAML.
    std::string match;                // Glob/literal applied to ctx["tool"].
    std::string command;              // Shell command or absolute path.
    std::filesystem::path source_dir; // Hook directory.
    std::filesystem::path manifest_path;
};

// Decoded JSON returned by a hook subprocess.
//   { "action": "continue" | "block", "message": "..." }
struct HookResult {
    enum class Action { Continue, Block };
    Action action = Action::Continue;
    std::string message;
    int exit_code = 0;
    std::string raw_stdout;
    std::string raw_stderr;
};

// Scan `${HERMES_HOME}/hooks` for both:
//   - top-level files named `*.{sh,py,yaml}`  (legacy single-file form -- only
//     `.yaml` files are parsed as manifests; `.sh` / `.py` are reported with
//     their basename as the name and event=Unknown so the caller can wire
//     them up by convention).
//   - subdirectories containing `HOOK.yaml`.
//
// Returns one HookManifest per parseable entry.  Parse errors are logged to
// stderr but never thrown -- discovery is best-effort.
std::vector<HookManifest> discover_hooks();

// Test seam: scan a custom root.
std::vector<HookManifest> discover_hooks_in(const std::filesystem::path& root);

// Filter helper -- returns hooks matching `event` whose `match` glob (if any)
// matches `tool_name`.  Empty `tool_name` skips the match check.
std::vector<HookManifest> select_matching(
    const std::vector<HookManifest>& hooks,
    HookEvent event,
    const std::string& tool_name = "");

// Spawn the hook subprocess.  `event_payload` is JSON-serialised and piped
// into the child's stdin.  The first valid JSON object on stdout is parsed
// into HookResult.  If parsing fails the result defaults to action=Continue
// with the raw stdout retained for diagnostics.
//
// `timeout` is wall-clock; the child is reaped (SIGKILL on POSIX) on expiry.
HookResult execute_hook(
    const HookManifest& hook,
    const nlohmann::json& event_payload,
    std::chrono::milliseconds timeout = std::chrono::seconds(30));

// Test seam -- inject a fake executor instead of spawning a real process.
// The fake receives (command, stdin_payload) and returns the (stdout, stderr,
// exit_code) tuple that execute_hook would have produced from a real spawn.
struct ExecutorInput {
    std::string command;
    std::string stdin_payload;
};
struct ExecutorOutput {
    std::string stdout_text;
    std::string stderr_text;
    int exit_code = 0;
};
using FakeExecutor = std::function<ExecutorOutput(const ExecutorInput&)>;

// When set, all execute_hook() calls are routed through this fake.  Pass
// nullptr to restore real execution.  Test-only.
void set_fake_executor(FakeExecutor fn);

}  // namespace hermes::cli
