// Display helpers — spinner animation and tool-call preview formatting.
#pragma once

#include "hermes/cli/skin_engine.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace hermes::cli {

class Spinner {
public:
    explicit Spinner(const SkinConfig& skin);
    ~Spinner();

    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;

    void start(const std::string& message = "");
    void stop();
    void update(const std::string& message);

private:
    void run();

    std::atomic<bool> running_{false};
    std::thread thread_;
    const SkinConfig& skin_;
    std::string message_;
    mutable std::mutex mu_;
};

// Build a compact preview string for a tool invocation, coloured per skin.
std::string build_tool_preview(const std::string& tool_name,
                               const nlohmann::json& args,
                               const std::string& result,
                               const SkinConfig& skin);

// Emoji for a tool name (e.g. "bash" -> wrench, "read_file" -> page).
std::string get_tool_emoji(const std::string& tool_name);

}  // namespace hermes::cli
