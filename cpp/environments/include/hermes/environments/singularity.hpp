// SingularityEnvironment — run commands inside an Apptainer/Singularity
// container.  Delegates process spawning to LocalEnvironment; the value
// added by this class is a) automatic binary detection (apptainer first,
// singularity second) and b) safe default argv assembly (containall,
// no-home, dropped capabilities, overlay, bind mounts, pwd, env).
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/local.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hermes::environments {

class SingularityEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string image = "docker://ubuntu:24.04";
        std::string binary = "apptainer";
        std::vector<std::string> bind_mounts;  // "host:container" entries
        bool containall = true;
        bool no_home = true;
        std::vector<std::string> capabilities_drop;
        std::filesystem::path overlay_dir;
        std::filesystem::path sif_cache;
        std::optional<std::string> task_id;
    };

    SingularityEnvironment();
    explicit SingularityEnvironment(Config config);
    ~SingularityEnvironment() override;

    std::string name() const override { return "singularity"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    // Tries "apptainer", then "singularity".  Returns the first binary
    // whose `--version` exits 0; falls back to "apptainer" when neither
    // is on PATH (the execute() call will then fail gracefully).
    static std::string detect_binary();

    // Assemble the argv (excluding `binary` itself) for invoking
    // `<binary> exec … <image> bash -c "<cmd>"`.  Exposed for tests.
    std::vector<std::string> build_singularity_args(
        const std::string& cmd, const ExecuteOptions& opts) const;

private:
    Config config_;
    LocalEnvironment local_;
};

}  // namespace hermes::environments
