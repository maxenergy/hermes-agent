// DockerEnvironment — run commands inside a Docker container.
//
// Delegates actual process spawning to LocalEnvironment — all we do is
// build the `docker run` command line with the right security flags.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/local.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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

    // Resolve an image reference (tag or digest) to a pinned
    // `repo@sha256:<digest>` form using `docker manifest inspect --verbose`.
    // Returns an empty string when the manifest cannot be fetched
    // (network failure, auth error, unknown tag).  Safe to call
    // repeatedly — it only invokes docker on cache miss.
    //
    // `image` is the raw ref (e.g. "ubuntu:24.04" or
    // "docker.io/library/ubuntu:latest" or "repo@sha256:abc...").  When
    // already digest-pinned the input is returned as-is.
    std::string resolve_image_digest(const std::string& image);

    // Pin the configured image in place.  Calls resolve_image_digest()
    // on config_.image and, on success, updates config_.image to the
    // digest-pinned form.  Idempotent.  Returns true on success.
    bool pin_configured_image();

    // Register an anonymous volume created by a `docker run` variant so
    // cleanup() can later prune it.  Normally --rm on `docker run`
    // already sweeps anonymous volumes, but named / labelled volumes
    // explicitly created via `docker volume create` should be tracked
    // here and removed at session end.
    void track_anonymous_volume(std::string volume_id);

    // Override the transport used to invoke docker for testing.  When
    // set, docker subcommands are shelled out via this callback instead
    // of the usual LocalEnvironment::execute.  The callback returns the
    // captured stdout plus the exit code via CompletedProcess.
    using DockerRunner = std::function<CompletedProcess(
        const std::vector<std::string>& argv)>;
    void set_docker_runner(DockerRunner runner) {
        runner_ = std::move(runner);
    }

private:
    // Default implementation: shell out via LocalEnvironment.
    CompletedProcess run_docker(const std::vector<std::string>& argv);

    Config config_;
    LocalEnvironment local_;
    std::string container_id_;
    std::vector<std::string> anonymous_volumes_;
    std::unordered_map<std::string, std::string> digest_cache_;  // ref -> pinned
    DockerRunner runner_;
};

}  // namespace hermes::environments
