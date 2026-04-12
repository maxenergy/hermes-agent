// DockerEnvironment — run commands inside a Docker container.
//
// Delegates actual process spawning to LocalEnvironment — all we do is
// build the `docker run` command line with the right security flags.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/local.hpp"

#include <optional>
#include <string>
#include <vector>

namespace hermes::environments {

class DockerEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string image = "ubuntu:24.04";
        std::optional<double> cpus;
        std::optional<std::string> memory;
        std::optional<int> pids_limit;
        std::vector<std::string> bind_mounts;
        std::vector<std::string> tmpfs_mounts;
        bool cap_drop_all = true;
        bool no_new_privileges = true;
    };

    DockerEnvironment();
    explicit DockerEnvironment(Config config);
    ~DockerEnvironment() override;

    std::string name() const override { return "docker"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    // Public for testing — returns the docker run argument vector
    // (everything between "docker" and the image name).
    std::vector<std::string> build_docker_args(
        const ExecuteOptions& opts) const;

private:
    Config config_;
    LocalEnvironment local_;
    std::string container_id_;
};

}  // namespace hermes::environments
