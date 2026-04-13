#include "hermes/environments/scp_copier.hpp"

#include <cstdlib>
#include <sstream>

namespace hermes::environments {

namespace {

std::string sq(const std::string& s) {
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

}  // namespace

std::vector<std::string> ScpCopier::build_argv(const fs::path& local,
                                               const fs::path& remote) const {
    std::vector<std::string> argv;
    argv.push_back(config_.scp_binary);
    // Preserve mtime + mode, batch mode (no prompts), quiet.
    argv.emplace_back("-p");
    argv.emplace_back("-B");
    argv.emplace_back("-q");
    if (config_.port > 0) {
        argv.emplace_back("-P");
        argv.emplace_back(std::to_string(config_.port));
    }
    for (const auto& opt : config_.ssh_options) {
        argv.emplace_back("-o");
        argv.emplace_back(opt);
    }
    if (!config_.control_path.empty()) {
        argv.emplace_back("-o");
        argv.emplace_back("ControlPath=" + config_.control_path);
    }
    argv.emplace_back(local.string());
    // scp target: user@host:remote-path
    std::string target = config_.target + ":" + remote.string();
    argv.emplace_back(target);
    return argv;
}

bool ScpCopier::copy(const fs::path& local, const fs::path& remote) const {
    auto argv = build_argv(local, remote);
    if (runner_) return runner_(argv) == 0;

    // Default runner — std::system with shell-quoted argv.
    std::ostringstream oss;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << ' ';
        oss << sq(argv[i]);
    }
    oss << " >/dev/null 2>&1";
    int rc = std::system(oss.str().c_str());
    return rc == 0;
}

std::function<bool(const fs::path&, const fs::path&)>
ScpCopier::as_copy_fn() const {
    // Capture by value so the callback outlives the caller's scope.
    Config cfg = config_;
    Runner runner = runner_;
    return [cfg, runner](const fs::path& local, const fs::path& remote) {
        ScpCopier c(cfg);
        if (runner) c.set_runner(runner);
        return c.copy(local, remote);
    };
}

}  // namespace hermes::environments
