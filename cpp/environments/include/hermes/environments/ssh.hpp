// SSHEnvironment — run commands on a remote host over SSH.
//
// Uses ControlMaster multiplexing for connection reuse.
// CWD tracking via MarkerCwdTracker (echo marker in stdout).
// Cleanup sends `ssh -O exit` to close the master connection.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/cwd_tracker.hpp"
#include "hermes/environments/local.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hermes::environments {

class SSHEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string host;
        std::string user;
        std::optional<int> port;
        std::optional<std::filesystem::path> identity_file;
        std::filesystem::path control_dir = "/tmp/hermes-ssh";
    };

    explicit SSHEnvironment(Config config);
    ~SSHEnvironment() override;

    std::string name() const override { return "ssh"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    // Public for testing — returns the ssh argument vector.
    std::vector<std::string> build_ssh_argv(const std::string& cmd) const;

private:
    Config config_;
    LocalEnvironment local_;
    MarkerCwdTracker cwd_tracker_;

    std::string control_socket_path() const;
};

}  // namespace hermes::environments
