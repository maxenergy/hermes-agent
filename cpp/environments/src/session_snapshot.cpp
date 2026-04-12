#include "hermes/environments/session_snapshot.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>

namespace hermes::environments {

namespace {

// Run a command and capture stdout.
std::string run_capture(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string result;
    FILE* raw = popen(cmd.c_str(), "r");
    if (!raw) return {};
    while (fgets(buf.data(), static_cast<int>(buf.size()), raw)) {
        result += buf.data();
    }
    pclose(raw);
    return result;
}

}  // namespace

SessionSnapshot capture_session(const std::string& shell) {
    SessionSnapshot snap;

    // Capture environment variables.
    std::string env_output = run_capture(shell + " -c 'env'");
    std::istringstream env_stream(env_output);
    std::string line;
    while (std::getline(env_stream, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            snap.env_vars.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
    }

    // Capture aliases.
    std::string alias_output = run_capture(shell + " -ic 'alias' 2>/dev/null");
    std::istringstream alias_stream(alias_output);
    while (std::getline(alias_stream, line)) {
        // alias name='value'
        if (line.substr(0, 6) == "alias ") {
            auto eq = line.find('=', 6);
            if (eq != std::string::npos) {
                std::string aname = line.substr(6, eq - 6);
                std::string aval = line.substr(eq + 1);
                // Strip surrounding quotes.
                if (aval.size() >= 2 && aval.front() == '\'' &&
                    aval.back() == '\'') {
                    aval = aval.substr(1, aval.size() - 2);
                }
                snap.aliases.emplace(std::move(aname), std::move(aval));
            }
        }
    }

    // Capture cwd.
    std::string cwd_str = run_capture("pwd");
    while (!cwd_str.empty() && (cwd_str.back() == '\n' ||
                                 cwd_str.back() == '\r')) {
        cwd_str.pop_back();
    }
    if (!cwd_str.empty()) {
        snap.cwd = cwd_str;
    }

    return snap;
}

std::string render_prelude(const SessionSnapshot& snap) {
    std::ostringstream oss;

    // Environment variables.
    for (const auto& [k, v] : snap.env_vars) {
        oss << "export " << k << "='" << v << "'\n";
    }

    // Shell functions.
    for (const auto& [name, body] : snap.shell_functions) {
        oss << name << "() {\n" << body << "\n}\n";
    }

    // Aliases.
    for (const auto& [name, value] : snap.aliases) {
        oss << "alias " << name << "='" << value << "'\n";
    }

    // Change directory.
    if (!snap.cwd.empty()) {
        oss << "cd '" << snap.cwd.string() << "'\n";
    }

    return oss.str();
}

}  // namespace hermes::environments
