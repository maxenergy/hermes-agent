#include "hermes/environments/singularity.hpp"

#include "hermes/environments/env_filter.hpp"

#include <cstdlib>
#include <sstream>

namespace hermes::environments {

namespace {

// Simple shell-quoting: wrap in single quotes, escape embedded quotes.
std::string shell_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

bool binary_exists(const std::string& name) {
    // Try `name --version >/dev/null 2>&1` via system().  A zero exit means
    // the binary is on PATH.
    std::string cmd = name + " --version >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

SingularityEnvironment::SingularityEnvironment() : config_() {
    config_.binary = detect_binary();
}

SingularityEnvironment::SingularityEnvironment(Config config)
    : config_(std::move(config)) {
    if (config_.binary.empty()) config_.binary = detect_binary();
}

SingularityEnvironment::~SingularityEnvironment() = default;

std::string SingularityEnvironment::detect_binary() {
    if (binary_exists("apptainer")) return "apptainer";
    if (binary_exists("singularity")) return "singularity";
    return "apptainer";
}

std::vector<std::string> SingularityEnvironment::build_singularity_args(
    const std::string& cmd, const ExecuteOptions& opts) const {
    std::vector<std::string> args;
    args.emplace_back("exec");

    if (config_.containall) args.emplace_back("--containall");
    if (config_.no_home) args.emplace_back("--no-home");

    if (!config_.capabilities_drop.empty()) {
        args.emplace_back("--drop-caps");
        std::string joined;
        for (std::size_t i = 0; i < config_.capabilities_drop.size(); ++i) {
            if (i > 0) joined += ",";
            joined += config_.capabilities_drop[i];
        }
        args.emplace_back(joined);
    }

    if (!config_.overlay_dir.empty()) {
        args.emplace_back("--overlay");
        args.emplace_back(config_.overlay_dir.string());
    }

    for (const auto& bind : config_.bind_mounts) {
        args.emplace_back("--bind");
        args.emplace_back(bind);
    }

    if (!opts.cwd.empty()) {
        args.emplace_back("--pwd");
        args.emplace_back(opts.cwd.string());
    }

    auto filtered = filter_env(opts.env_vars);
    for (const auto& [k, v] : filtered) {
        args.emplace_back("--env");
        args.emplace_back(k + "=" + v);
    }

    args.emplace_back(config_.image);
    args.emplace_back("bash");
    args.emplace_back("-c");
    args.emplace_back(cmd);

    return args;
}

CompletedProcess SingularityEnvironment::execute(const std::string& cmd,
                                                 const ExecuteOptions& opts) {
    auto args = build_singularity_args(cmd, opts);

    std::ostringstream full_cmd;
    full_cmd << shell_quote(config_.binary);
    for (const auto& a : args) {
        full_cmd << ' ' << shell_quote(a);
    }

    // SIF cache as env var for the child.
    ExecuteOptions local_opts;
    local_opts.timeout = opts.timeout;
    local_opts.cancel_fn = opts.cancel_fn;
    if (!config_.sif_cache.empty()) {
        local_opts.env_vars["APPTAINER_CACHEDIR"] = config_.sif_cache.string();
        local_opts.env_vars["SINGULARITY_CACHEDIR"] = config_.sif_cache.string();
    }

    return local_.execute(full_cmd.str(), local_opts);
}

void SingularityEnvironment::cleanup() {
    // Nothing persistent — containers are ephemeral per-exec.
}

}  // namespace hermes::environments
