// LocalEnvironment — run commands locally via fork+exec of bash -c.
//
// Timeout: a watchdog thread sends SIGTERM, waits 2 s, then SIGKILL.
// Cancellation: cancel_fn is polled every 100 ms.
// CWD tracking: FileCwdTracker (writes cwd to a temp file).
// PTY: guarded by __linux__ / forkpty availability; fallback to pipe.
// Windows: throws std::runtime_error.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/cwd_tracker.hpp"

#include <memory>

namespace hermes::environments {

class LocalEnvironment : public BaseEnvironment {
public:
    LocalEnvironment();
    ~LocalEnvironment() override;

    std::string name() const override { return "local"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override {}

private:
    std::unique_ptr<FileCwdTracker> cwd_tracker_;
};

}  // namespace hermes::environments
