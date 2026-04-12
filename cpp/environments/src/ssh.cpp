#include "hermes/environments/ssh.hpp"

#include <sstream>

namespace hermes::environments {

namespace fs = std::filesystem;

SSHEnvironment::SSHEnvironment(Config config)
    : config_(std::move(config)) {}

SSHEnvironment::~SSHEnvironment() {
    cleanup();
}

std::string SSHEnvironment::control_socket_path() const {
    return (config_.control_dir / ("hermes-" + config_.user + "@" +
                                    config_.host))
        .string();
}

std::vector<std::string> SSHEnvironment::build_ssh_argv(
    const std::string& cmd) const {
    std::vector<std::string> argv;

    argv.emplace_back("ssh");

    // ControlMaster flags.
    argv.emplace_back("-o");
    argv.emplace_back("ControlMaster=auto");
    argv.emplace_back("-o");
    argv.emplace_back("ControlPath=" + control_socket_path());
    argv.emplace_back("-o");
    argv.emplace_back("ControlPersist=60");

    // Port.
    if (config_.port) {
        argv.emplace_back("-p");
        argv.emplace_back(std::to_string(*config_.port));
    }

    // Identity file.
    if (config_.identity_file) {
        argv.emplace_back("-i");
        argv.emplace_back(config_.identity_file->string());
    }

    // Disable strict host key checking for automation.
    argv.emplace_back("-o");
    argv.emplace_back("StrictHostKeyChecking=no");
    argv.emplace_back("-o");
    argv.emplace_back("BatchMode=yes");

    // User@host.
    std::string target = config_.user.empty()
                             ? config_.host
                             : config_.user + "@" + config_.host;
    argv.emplace_back(target);

    // Command.
    argv.emplace_back(cmd);

    return argv;
}

CompletedProcess SSHEnvironment::execute(const std::string& cmd,
                                         const ExecuteOptions& opts) {
    // Ensure control directory exists.
    std::error_code ec;
    fs::create_directories(config_.control_dir, ec);

    // Wrap command with MarkerCwdTracker.
    auto cwd = opts.cwd.empty() ? fs::path("/") : opts.cwd;
    std::string wrapped = cwd_tracker_.before_run(cmd, cwd);

    // Build cd prefix if cwd is set.
    std::string remote_cmd;
    if (!opts.cwd.empty()) {
        remote_cmd = "cd '" + opts.cwd.string() + "' && " + wrapped;
    } else {
        remote_cmd = wrapped;
    }

    auto argv = build_ssh_argv(remote_cmd);

    // Build the shell command from argv.
    std::ostringstream shell_cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) shell_cmd << " ";
        shell_cmd << "'" << argv[i] << "'";
    }

    ExecuteOptions local_opts;
    local_opts.timeout = opts.timeout;
    local_opts.cancel_fn = opts.cancel_fn;

    auto result = local_.execute(shell_cmd.str(), local_opts);

    // Extract cwd from marker.
    result.final_cwd = cwd_tracker_.after_run(result.stdout_text);

    return result;
}

void SSHEnvironment::cleanup() {
    // Close the ControlMaster connection.
    std::string target = config_.user.empty()
                             ? config_.host
                             : config_.user + "@" + config_.host;
    std::string cmd = "ssh -o ControlPath=" + control_socket_path() +
                      " -O exit " + target + " 2>/dev/null";
    int rc = ::system(cmd.c_str());
    (void)rc;
}

}  // namespace hermes::environments
