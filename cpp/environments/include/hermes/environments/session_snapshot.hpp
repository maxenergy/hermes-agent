// SessionSnapshot — capture and render a bash session's environment,
// functions, aliases and cwd so that a fresh shell can reproduce them.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>

namespace hermes::environments {

struct SessionSnapshot {
    std::unordered_map<std::string, std::string> env_vars;
    std::map<std::string, std::string> shell_functions;
    std::map<std::string, std::string> aliases;
    std::filesystem::path cwd;
};

// Capture the current session state by spawning `shell` with appropriate
// introspection commands.  Returns a best-effort snapshot; fields that
// could not be captured are left empty.
SessionSnapshot capture_session(const std::string& shell = "bash");

// Render a shell prelude (sequence of export / function / alias / cd
// statements) that, when sourced, approximates the captured snapshot.
std::string render_prelude(const SessionSnapshot& snap);

}  // namespace hermes::environments
