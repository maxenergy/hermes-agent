// BaseEnvironment — abstract interface for command execution environments.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

#include "hermes/environments/completed_process.hpp"

namespace hermes::environments {

struct ExecuteOptions {
    std::filesystem::path cwd;
    std::unordered_map<std::string, std::string> env_vars;
    std::chrono::seconds timeout{600};
    bool use_pty = false;
    std::function<bool()> cancel_fn;
};

class BaseEnvironment {
public:
    virtual ~BaseEnvironment() = default;

    virtual std::string name() const = 0;

    virtual CompletedProcess execute(const std::string& cmd,
                                     const ExecuteOptions& opts) = 0;

    virtual void cleanup() {}
};

}  // namespace hermes::environments
