// ScpCopier — copy local files to a remote SSH host using the `scp`
// subprocess.  We intentionally do NOT link against libssh2 here: the
// build environment may lack it, and `scp` is universally available
// everywhere OpenSSH is.  Callers who need programmatic control can
// construct a ScpCopier with their own argv prefix (e.g. forcing a
// specific identity or ControlPath) and hand the resulting `CopyFn` to
// FileSyncManager::sync_to_remote().
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hermes::environments {

namespace fs = std::filesystem;

class ScpCopier {
public:
    struct Config {
        // Target host, either "user@host" or just "host".
        std::string target;
        // Optional port override.
        int port = 0;
        // Extra options passed via `-o`, e.g. "StrictHostKeyChecking=accept-new".
        std::vector<std::string> ssh_options;
        // Binary name — "scp" by default.  Tests override this.
        std::string scp_binary = "scp";
        // Re-use an SSH ControlMaster socket.  When non-empty, `-o
        // ControlPath=<path>` is added so scp shares a multiplexed
        // session with the active SSHEnvironment.
        std::string control_path;
    };

    ScpCopier() = default;
    explicit ScpCopier(Config config) : config_(std::move(config)) {}

    // Build the argv that would be exec'd to copy `local` → `remote`.
    // Exposed for testing — the actual runner is a thin std::system
    // wrapper on top of this.
    std::vector<std::string> build_argv(const fs::path& local,
                                        const fs::path& remote) const;

    // Copy a single file.  Returns true on zero exit status.  On
    // platforms without scp or when the subprocess fails, returns false.
    bool copy(const fs::path& local, const fs::path& remote) const;

    // Returns a `FileSyncManager::CopyFn`-shaped callback bound to this
    // copier.  Useful when passing into FileSyncManager::sync_to_remote.
    std::function<bool(const fs::path&, const fs::path&)> as_copy_fn() const;

    // Override the runner for tests — receives the full argv and
    // returns the would-be exit code.
    using Runner = std::function<int(const std::vector<std::string>&)>;
    void set_runner(Runner r) { runner_ = std::move(r); }

private:
    Config config_;
    Runner runner_;
};

}  // namespace hermes::environments
