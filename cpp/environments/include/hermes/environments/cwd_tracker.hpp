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

}  // namespace hermes::environments
