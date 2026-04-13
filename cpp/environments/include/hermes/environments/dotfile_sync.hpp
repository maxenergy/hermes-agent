// DotfileManager — stage & upload user dotfiles (.gitconfig, .bashrc,
// ~/.ssh/config, etc.) to a remote environment via a caller-supplied
// copy callback.
//
// Motivation: remote sandboxes (Docker/Modal/Daytona/Singularity/SSH)
// start from a pristine image — git user, shell aliases, ssh hosts are
// all missing.  Python's environments/*.py implementation mirrors a
// sanitized subset of ~/.  The same semantics live here.
//
// SSH configs are sanitized before upload: `IdentityFile`, `CertificateFile`
// and `ControlPath` lines are stripped (they reference local paths and
// could leak secrets).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hermes::environments {

namespace fs = std::filesystem;

class DotfileManager {
public:
    // Payload ready for remote upload.  `local_path` may be a
    // synthesized temp file for sanitized content — callers can ignore
    // that detail and simply copy `local_path` → `remote_path`.
    struct Staged {
        fs::path local_path;
        fs::path remote_path;
        bool sanitized = false;
    };

    struct Config {
        // Home directory to read from.  Defaults to $HOME.
        fs::path local_home;
        // Remote $HOME (e.g. /root on many container images).  Defaults
        // to "/root".
        fs::path remote_home = "/root";
        // Which files to attempt.  Empty = use the default set:
        //   .gitconfig
        //   .bashrc
        //   .bash_profile
        //   .zshrc
        //   .inputrc
        //   .tmux.conf
        //   .vimrc
        //   .config/git/config
        //   .ssh/config       (sanitized)
        //   .ssh/known_hosts
        std::vector<std::string> files;
    };

    DotfileManager();
    explicit DotfileManager(Config config);
    ~DotfileManager();

    // Prepare (but do not upload) the set of dotfiles that exist
    // locally.  Sanitized payloads are written to a managed temp dir
    // whose lifetime is tied to this DotfileManager.
    std::vector<Staged> stage();

    using CopyFn =
        std::function<bool(const fs::path& local, const fs::path& remote)>;

    // Stage + upload via the callback.  Returns the number of files
    // successfully transferred.  Missing source files are skipped
    // silently (common for .zshrc on bash-only systems, etc.).
    std::size_t upload(CopyFn copy_fn);

    // Expose the default file list so callers/tests can inspect it.
    static std::vector<std::string> default_files();

    // Sanitize an SSH config payload — strip IdentityFile /
    // CertificateFile / ControlPath / ControlMaster directives.
    // Exposed for testing.
    static std::string sanitize_ssh_config(const std::string& input);

private:
    Config config_;
    fs::path tmp_root_;
};

}  // namespace hermes::environments
