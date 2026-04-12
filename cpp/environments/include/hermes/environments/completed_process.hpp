// CompletedProcess — result of running a command in any environment.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace hermes::environments {

struct CompletedProcess {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    std::chrono::milliseconds duration{0};
    bool timed_out = false;
    bool interrupted = false;
    std::filesystem::path final_cwd;
};

}  // namespace hermes::environments
