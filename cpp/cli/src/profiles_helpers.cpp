// Pure-logic helpers ported from `hermes_cli/profiles.py`.
#include "hermes/cli/profiles_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace hermes::cli::profiles_helpers {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    size_t i {0};
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& reserved_names() {
    static const std::unordered_set<std::string> names {
        "hermes", "default", "test", "tmp", "root", "sudo",
    };
    return names;
}

const std::unordered_set<std::string>& hermes_subcommands() {
    static const std::unordered_set<std::string> cmds {
        "chat", "model", "gateway", "setup", "whatsapp", "login", "logout",
        "status", "cron", "doctor", "dump", "config", "pairing", "skills",
        "tools", "mcp", "sessions", "insights", "version", "update",
        "uninstall", "profile", "plugins", "honcho", "acp",
    };
    return cmds;
}

// ---------------------------------------------------------------------------

bool is_valid_profile_id(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const unsigned char first {static_cast<unsigned char>(name.front())};
    if (!std::isalnum(first) || (std::isupper(first))) {
        return false;
    }
    if (std::isalpha(first) && !std::islower(first)) {
        return false;
    }
    for (char c : name) {
        const unsigned char u {static_cast<unsigned char>(c)};
        const bool ok {
            std::isdigit(u)
            || (std::isalpha(u) && std::islower(u))
            || c == '_' || c == '-'};
        if (!ok) {
            return false;
        }
    }
    return true;
}

void validate_profile_name(const std::string& name) {
    if (name == "default") {
        return;
    }
    if (!is_valid_profile_id(name)) {
        throw std::invalid_argument {
            "Invalid profile name '" + name +
            "'. Must match [a-z0-9][a-z0-9_-]{0,63}"};
    }
}

std::optional<std::string> check_alias_collision(
    const std::string& name,
    const std::vector<std::string>& existing_commands,
    const std::vector<std::string>& own_wrapper_paths) {
    if (reserved_names().count(name) > 0) {
        return std::string {"'"} + name + "' is a reserved name";
    }
    if (hermes_subcommands().count(name) > 0) {
        return std::string {"'"} + name + "' conflicts with a hermes subcommand";
    }
    for (const std::string& cmd : existing_commands) {
        // Own wrappers may be overwritten.
        const bool is_own {std::find(own_wrapper_paths.begin(),
                                     own_wrapper_paths.end(),
                                     cmd) != own_wrapper_paths.end()};
        if (is_own) {
            continue;
        }
        return std::string {"'"} + name +
               "' conflicts with an existing command (" + cmd + ")";
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------

fs::path get_profile_dir(
    const std::string& name,
    const fs::path& default_root) {
    if (name == "default") {
        return default_root;
    }
    return default_root / "profiles" / name;
}

fs::path get_profiles_root(const fs::path& default_root) {
    return default_root / "profiles";
}

fs::path get_active_profile_path(const fs::path& default_root) {
    return default_root / "active_profile";
}

// ---------------------------------------------------------------------------

std::string render_wrapper_script(const std::string& profile_name) {
    return std::string {"#!/bin/sh\nexec hermes -p "} + profile_name +
           " \"$@\"\n";
}

bool is_hermes_wrapper(const std::string& script_text) {
    return script_text.find("hermes -p") != std::string::npos;
}

// ---------------------------------------------------------------------------

std::vector<std::string> normalize_archive_member(
    const std::string& member_name) {
    if (member_name.empty()) {
        throw std::invalid_argument {
            "Unsafe archive member path: <empty>"};
    }
    // Reject Windows absolute paths / drive letters.
    if (member_name.size() >= 2 && member_name[1] == ':') {
        throw std::invalid_argument {
            "Unsafe archive member path: " + member_name};
    }
    // Normalise backslashes to forward slashes.
    std::string normalised;
    normalised.reserve(member_name.size());
    for (char c : member_name) {
        normalised.push_back(c == '\\' ? '/' : c);
    }
    if (!normalised.empty() && normalised.front() == '/') {
        throw std::invalid_argument {
            "Unsafe archive member path: " + member_name};
    }
    // Split on '/' and filter empty / ".".
    std::vector<std::string> parts;
    std::string cur;
    for (char c : normalised) {
        if (c == '/') {
            if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
    }
    if (parts.empty()) {
        throw std::invalid_argument {
            "Unsafe archive member path: " + member_name};
    }
    for (const std::string& p : parts) {
        if (p == "..") {
            throw std::invalid_argument {
                "Unsafe archive member path: " + member_name};
        }
    }
    return parts;
}

// ---------------------------------------------------------------------------

std::optional<int> parse_gateway_pid_file(const std::string& text) {
    const std::string trimmed {trim(text)};
    if (trimmed.empty()) {
        return std::nullopt;
    }
    if (trimmed.front() == '{') {
        // Minimal JSON pid extraction: find "pid": N
        const std::regex re {R"("pid"\s*:\s*(\d+))"};
        std::smatch m;
        if (std::regex_search(trimmed, m, re)) {
            try {
                return std::stoi(m[1].str());
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    for (char c : trimmed) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    try {
        return std::stoi(trimmed);
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------

std::string generate_bash_completion() {
    return R"SCRIPT(# Hermes Agent profile completion
# Add to ~/.bashrc: eval "$(hermes completion bash)"

_hermes_profiles() {
    local profiles_dir="$HOME/.hermes/profiles"
    local profiles="default"
    if [ -d "$profiles_dir" ]; then
        profiles="$profiles $(ls "$profiles_dir" 2>/dev/null)"
    fi
    echo "$profiles"
}

_hermes_completion() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    if [[ "$prev" == "-p" || "$prev" == "--profile" ]]; then
        COMPREPLY=($(compgen -W "$(_hermes_profiles)" -- "$cur"))
        return
    fi

    if [[ "${COMP_WORDS[1]}" == "profile" ]]; then
        case "$prev" in
            profile)
                COMPREPLY=($(compgen -W "list use create delete show alias rename export import" -- "$cur"))
                return
                ;;
            use|delete|show|alias|rename|export)
                COMPREPLY=($(compgen -W "$(_hermes_profiles)" -- "$cur"))
                return
                ;;
        esac
    fi

    if [[ "$COMP_CWORD" == 1 ]]; then
        local commands="chat model gateway setup status cron doctor dump config skills tools mcp sessions profile update version"
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
    fi
}

complete -F _hermes_completion hermes
)SCRIPT";
}

std::string generate_zsh_completion() {
    return R"SCRIPT(#compdef hermes
# Hermes Agent profile completion
# Add to ~/.zshrc: eval "$(hermes completion zsh)"

_hermes() {
    local -a profiles
    profiles=(default)
    if [[ -d "$HOME/.hermes/profiles" ]]; then
        profiles+=("${(@f)$(ls $HOME/.hermes/profiles 2>/dev/null)}")
    fi

    _arguments \
        '-p[Profile name]:profile:($profiles)' \
        '--profile[Profile name]:profile:($profiles)' \
        '1:command:(chat model gateway setup status cron doctor dump config skills tools mcp sessions profile update version)' \
        '*::arg:->args'

    case $words[1] in
        profile)
            _arguments '1:action:(list use create delete show alias rename export import)' \
                        '2:profile:($profiles)'
            ;;
    esac
}

_hermes "$@"
)SCRIPT";
}

// ---------------------------------------------------------------------------

bool is_default_export_excluded(const std::string& filename) {
    static const std::unordered_set<std::string> excluded {
        "hermes-agent", ".worktrees", "profiles", "bin", "node_modules",
        "state.db", "state.db-shm", "state.db-wal",
        "hermes_state.db",
        "response_store.db", "response_store.db-shm", "response_store.db-wal",
        "gateway.pid", "gateway_state.json", "processes.json",
        "auth.json", ".env",
        "auth.lock", "active_profile", ".update_check",
        "errors.log", ".hermes_history",
        "image_cache", "audio_cache", "document_cache",
        "browser_screenshots", "checkpoints", "sandboxes", "logs",
    };
    return excluded.count(filename) > 0;
}

// ---------------------------------------------------------------------------

void validate_rename(const std::string& old_name,
                     const std::string& new_name) {
    validate_profile_name(old_name);
    validate_profile_name(new_name);
    if (old_name == "default") {
        throw std::invalid_argument {
            "Cannot rename the default profile."};
    }
    if (new_name == "default") {
        throw std::invalid_argument {
            "Cannot rename to 'default' — it is reserved."};
    }
    if (old_name == new_name) {
        throw std::invalid_argument {
            "Old and new names are the same."};
    }
}

}  // namespace hermes::cli::profiles_helpers
