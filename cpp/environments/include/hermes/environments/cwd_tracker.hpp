// CwdTracker — track the current working directory across shell invocations.
//
// Two strategies:
//   FileCwdTracker  — write cwd to a temp file after each command.
//   MarkerCwdTracker — append `echo "__HERMES_CWD__=$(pwd)"` and parse stdout.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace hermes::environments {

namespace fs = std::filesystem;

class CwdTracker {
public:
    virtual ~CwdTracker() = default;

    // May modify `cmd` (by appending a cwd-capture suffix).
    // `cwd` is the working directory the command will run in.
    // Returns the (possibly modified) command string.
    virtual std::string before_run(const std::string& cmd,
                                   const fs::path& cwd) = 0;

    // Inspect `stdout_text` (possibly stripping the marker line) and
    // return the detected cwd. Returns empty path on failure.
    virtual fs::path after_run(std::string& stdout_text) = 0;
};

// Writes the cwd to a temporary file after the command runs.
// The temp file path is injected via before_run; after_run reads it.
class FileCwdTracker : public CwdTracker {
public:
    FileCwdTracker();
    ~FileCwdTracker() override;

    std::string before_run(const std::string& cmd,
                           const fs::path& cwd) override;
    fs::path after_run(std::string& stdout_text) override;

private:
    fs::path tmp_file_;
};

// Appends `echo "__HERMES_CWD__=$(pwd)"` to the command and strips it
// from stdout after execution.
class MarkerCwdTracker : public CwdTracker {
public:
    static constexpr const char* kMarker = "__HERMES_CWD__=";

    std::string before_run(const std::string& cmd,
                           const fs::path& cwd) override;
    fs::path after_run(std::string& stdout_text) override;
};

// Parses in-band OSC 7 ("set window title path") sequences:
//     ESC ] 7 ; file://<host>/<absolute-path> BEL
//     ESC ] 7 ; file://<host>/<absolute-path> ESC backslash
// which are emitted automatically by modern terminals and many shells
// (bash via PROMPT_COMMAND=__vte_prompt_command, zsh vcs_info, fish, etc.)
// as the user navigates the filesystem. Unlike the other trackers this
// one does not modify the command — it merely watches the output for
// already-present OSC 7 markers.
//
// After successful parsing, the OSC sequence is stripped from
// `stdout_text` so downstream consumers do not see it.
class OscCwdTracker : public CwdTracker {
public:
    // `before_run` is a no-op — OSC 7 is shell-driven, not injected.
    std::string before_run(const std::string& cmd,
                           const fs::path& cwd) override {
        (void)cwd;
        return cmd;
    }
    fs::path after_run(std::string& stdout_text) override;

    // Stateless helper: parse the *last* OSC 7 sequence in `text`.
    // Returns empty path if none found. Does NOT mutate `text`.
    static fs::path parse_last(std::string_view text);

    // Strip every OSC 7 sequence from `text` in place.
    static void strip_osc7(std::string& text);
};

}  // namespace hermes::environments
