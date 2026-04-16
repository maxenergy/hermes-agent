#include <hermes/cli/hook_discovery.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <yaml-cpp/yaml.h>

#include <hermes/core/path.hpp>
#include <hermes/core/platform/subprocess.hpp>

namespace hermes::cli {

namespace {

namespace fs = std::filesystem;

FakeExecutor& fake_executor() {
    static FakeExecutor fn;
    return fn;
}

// Map a manifest event string to the enum.  Accepts both hyphen and
// underscore and `:` form for symmetry with the gateway's HookRegistry.
HookEvent event_from_str(const std::string& s) {
    auto norm = s;
    std::transform(norm.begin(), norm.end(), norm.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (auto& c : norm) {
        if (c == '_' || c == ':') c = '-';
    }
    if (norm == "pre-tool" || norm == "pretool") return HookEvent::PreTool;
    if (norm == "post-tool" || norm == "posttool") return HookEvent::PostTool;
    if (norm == "session-start") return HookEvent::SessionStart;
    if (norm == "session-end") return HookEvent::SessionEnd;
    if (norm == "user-prompt" || norm == "userprompt") return HookEvent::UserPrompt;
    return HookEvent::Unknown;
}

// Minimal glob match supporting `*` and `?`, plus exact match.  Empty
// pattern means "match everything".
bool glob_match(const std::string& pattern, const std::string& text) {
    if (pattern.empty() || pattern == "*") return true;

    // Iterative wildcard match.
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

std::optional<HookManifest> load_manifest(const fs::path& manifest_path,
                                         const fs::path& source_dir) {
    YAML::Node node;
    try {
        node = YAML::LoadFile(manifest_path.string());
    } catch (const std::exception& e) {
        std::cerr << "[hooks] failed to parse " << manifest_path << ": "
                  << e.what() << "\n";
        return std::nullopt;
    }

    if (!node || !node.IsMap()) {
        std::cerr << "[hooks] " << manifest_path
                  << ": top-level must be a YAML mapping\n";
        return std::nullopt;
    }

    HookManifest m;
    m.source_dir = source_dir;
    m.manifest_path = manifest_path;
    if (node["name"]) m.name = node["name"].as<std::string>("");
    if (m.name.empty()) m.name = source_dir.filename().string();

    if (node["event"]) {
        m.event_raw = node["event"].as<std::string>("");
        m.event = event_from_str(m.event_raw);
    }
    if (node["match"]) m.match = node["match"].as<std::string>("");
    if (node["command"]) m.command = node["command"].as<std::string>("");

    // Resolve relative `command` against the manifest's directory so users
    // can write `command: ./run.sh` next to their script.
    if (!m.command.empty()) {
        fs::path cmd_path(m.command);
        if (cmd_path.is_relative() && cmd_path.has_parent_path() == false) {
            // Single token (e.g. "run.sh") — try resolving against source_dir.
            auto candidate = source_dir / m.command;
            if (fs::exists(candidate)) {
                m.command = candidate.string();
            }
        }
    }

    return m;
}

// Spawn the child for real — goes through the cross-platform subprocess
// primitive, which delegates to fork+execvp on POSIX and CreateProcessW
// on Win32.  We still shell out via "/bin/sh -c" (or "cmd /c") so the
// HOOK.yaml `command` value can be a shell pipeline.
ExecutorOutput spawn_real(const std::string& command,
                          const std::string& stdin_payload,
                          std::chrono::milliseconds timeout) {
    ExecutorOutput out;
    hermes::core::platform::SubprocessOptions opts;
#if defined(_WIN32)
    opts.argv = {"cmd.exe", "/c", command};
#else
    opts.argv = {"/bin/sh", "-c", command};
#endif
    opts.stdin_input = stdin_payload;
    opts.timeout = timeout;

    auto r = hermes::core::platform::run_capture(opts);
    if (!r.spawn_error.empty()) {
        out.stderr_text = r.spawn_error;
        out.exit_code = 127;
        return out;
    }
    out.stdout_text = std::move(r.stdout_text);
    out.stderr_text = std::move(r.stderr_text);
    if (r.timed_out) {
        out.exit_code = 124;  // Mirror coreutils `timeout`.
        out.stderr_text += "[hooks] subprocess timed out\n";
    } else {
        out.exit_code = r.exit_code;
    }
    return out;
}

}  // namespace

HookEvent parse_event(const std::string& s) { return event_from_str(s); }

std::string event_to_string(HookEvent e) {
    switch (e) {
        case HookEvent::PreTool: return "pre-tool";
        case HookEvent::PostTool: return "post-tool";
        case HookEvent::SessionStart: return "session-start";
        case HookEvent::SessionEnd: return "session-end";
        case HookEvent::UserPrompt: return "user-prompt";
        case HookEvent::Unknown: default: return "unknown";
    }
}

std::vector<HookManifest> discover_hooks() {
    return discover_hooks_in(hermes::core::path::get_hermes_home() / "hooks");
}

std::vector<HookManifest> discover_hooks_in(const fs::path& root) {
    std::vector<HookManifest> out;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return out;
    }

    // Iterate sorted for deterministic ordering.
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(root, ec)) {
        entries.push_back(e.path());
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& entry : entries) {
        std::error_code is_dir_ec;
        if (fs::is_directory(entry, is_dir_ec)) {
            auto manifest = entry / "HOOK.yaml";
            if (fs::exists(manifest)) {
                if (auto m = load_manifest(manifest, entry)) {
                    out.push_back(std::move(*m));
                }
            }
            continue;
        }
        // File entry — accept .yaml as a standalone manifest.
        auto ext = entry.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".yaml" || ext == ".yml") {
            if (auto m = load_manifest(entry, entry.parent_path())) {
                out.push_back(std::move(*m));
            }
        } else if (ext == ".sh" || ext == ".py") {
            // Bare scripts -- record with Unknown event so the caller can
            // wire them up by convention or ignore them.
            HookManifest m;
            m.name = entry.stem().string();
            m.command = entry.string();
            m.source_dir = entry.parent_path();
            m.manifest_path = entry;
            m.event = HookEvent::Unknown;
            out.push_back(std::move(m));
        }
    }
    return out;
}

std::vector<HookManifest> select_matching(
    const std::vector<HookManifest>& hooks,
    HookEvent event,
    const std::string& tool_name) {
    std::vector<HookManifest> out;
    for (const auto& h : hooks) {
        if (h.event != event) continue;
        if (!tool_name.empty() && !h.match.empty() &&
            !glob_match(h.match, tool_name)) {
            continue;
        }
        out.push_back(h);
    }
    return out;
}

HookResult execute_hook(const HookManifest& hook,
                        const nlohmann::json& event_payload,
                        std::chrono::milliseconds timeout) {
    HookResult result;
    if (hook.command.empty()) {
        result.action = HookResult::Action::Continue;
        result.message = "[hooks] '" + hook.name + "' has no command";
        return result;
    }

    auto stdin_payload = event_payload.dump();

    ExecutorOutput exec_out;
    if (fake_executor()) {
        exec_out = fake_executor()(ExecutorInput{hook.command, stdin_payload});
    } else {
        exec_out = spawn_real(hook.command, stdin_payload, timeout);
    }

    result.exit_code = exec_out.exit_code;
    result.raw_stdout = exec_out.stdout_text;
    result.raw_stderr = exec_out.stderr_text;

    // Try to parse a JSON object out of stdout.  Tolerate leading/trailing
    // whitespace + stray log lines: locate the first '{' and the matching '}'.
    auto try_parse = [&](const std::string& s) -> bool {
        auto start = s.find('{');
        if (start == std::string::npos) return false;
        auto end = s.rfind('}');
        if (end == std::string::npos || end < start) return false;
        try {
            auto j = nlohmann::json::parse(s.substr(start, end - start + 1));
            if (!j.is_object()) return false;
            auto action = j.value("action", std::string("continue"));
            result.action = (action == "block") ? HookResult::Action::Block
                                                : HookResult::Action::Continue;
            result.message = j.value("message", std::string{});
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    if (!try_parse(exec_out.stdout_text)) {
        // No JSON: nonzero exit -> block, zero exit -> continue.
        result.action = (result.exit_code == 0)
                            ? HookResult::Action::Continue
                            : HookResult::Action::Block;
    }
    return result;
}

void set_fake_executor(FakeExecutor fn) { fake_executor() = std::move(fn); }

}  // namespace hermes::cli
