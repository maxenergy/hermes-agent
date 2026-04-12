#include "hermes/environments/docker.hpp"
#include "hermes/environments/env_filter.hpp"

#include <sstream>

namespace hermes::environments {

DockerEnvironment::DockerEnvironment() : config_() {}

DockerEnvironment::DockerEnvironment(Config config)
    : config_(std::move(config)) {}

DockerEnvironment::~DockerEnvironment() = default;

std::vector<std::string> DockerEnvironment::build_docker_args(
    const ExecuteOptions& opts) const {
    std::vector<std::string> args;

    args.emplace_back("run");
    args.emplace_back("--rm");
    args.emplace_back("-i");

    // Security flags.
    if (config_.cap_drop_all) {
        args.emplace_back("--cap-drop=ALL");
    }
    if (config_.no_new_privileges) {
        args.emplace_back("--security-opt");
        args.emplace_back("no-new-privileges");
    }

    // Resource limits.
    if (config_.pids_limit) {
        args.emplace_back("--pids-limit");
        args.emplace_back(std::to_string(*config_.pids_limit));
    }
    if (config_.cpus) {
        args.emplace_back("--cpus");
        // Format with reasonable precision.
        std::ostringstream oss;
        oss << *config_.cpus;
        args.emplace_back(oss.str());
    }
    if (config_.memory) {
        args.emplace_back("--memory");
        args.emplace_back(*config_.memory);
    }

    // Mounts.
    for (const auto& mount : config_.bind_mounts) {
        args.emplace_back("-v");
        args.emplace_back(mount);
    }
    for (const auto& tmpfs : config_.tmpfs_mounts) {
        args.emplace_back("--tmpfs");
        args.emplace_back(tmpfs);
    }

    // Working directory.
    if (!opts.cwd.empty()) {
        args.emplace_back("-w");
        args.emplace_back(opts.cwd.string());
    }

    // Filtered environment variables.
    auto filtered = filter_env(opts.env_vars);
    for (const auto& [k, v] : filtered) {
        args.emplace_back("-e");
        args.emplace_back(k + "=" + v);
    }

    return args;
}

CompletedProcess DockerEnvironment::execute(const std::string& cmd,
                                            const ExecuteOptions& opts) {
    auto docker_args = build_docker_args(opts);

    // Build the full command line.
    std::ostringstream full_cmd;
    full_cmd << "docker";
    for (const auto& arg : docker_args) {
        full_cmd << " '" << arg << "'";
    }
    full_cmd << " '" << config_.image << "'";
    full_cmd << " bash -c '" << cmd << "'";

    ExecuteOptions local_opts;
    local_opts.timeout = opts.timeout;
    local_opts.cancel_fn = opts.cancel_fn;
    // Don't pass env or cwd — those are in the docker args.

    return local_.execute(full_cmd.str(), local_opts);
}

void DockerEnvironment::cleanup() {
    // If we had a persistent container, stop it here.
    // For --rm containers, nothing to do.
}

}  // namespace hermes::environments
