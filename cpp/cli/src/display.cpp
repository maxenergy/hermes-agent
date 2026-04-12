#include "hermes/cli/display.hpp"

#include <chrono>
#include <iostream>

namespace hermes::cli {

Spinner::Spinner(const SkinConfig& skin) : skin_(skin) {}

Spinner::~Spinner() {
    stop();
}

void Spinner::start(const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        message_ = message;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::thread(&Spinner::run, this);
}

void Spinner::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    // Clear the spinner line.
    std::cerr << "\r\033[K" << std::flush;
}

void Spinner::update(const std::string& message) {
    std::lock_guard<std::mutex> lk(mu_);
    message_ = message;
}

void Spinner::run() {
    const auto& faces = skin_.spinner.thinking_faces;
    const auto& verbs = skin_.spinner.thinking_verbs;
    const auto& wings = skin_.spinner.wings;
    std::size_t idx = 0;
    while (running_.load()) {
        std::string face = faces.empty() ? "..." : faces[idx % faces.size()];
        std::string verb = verbs.empty() ? "thinking" : verbs[idx % verbs.size()];
        std::string left, right;
        if (!wings.empty()) {
            const auto& w = wings[idx % wings.size()];
            left = w.first;
            right = w.second;
        }

        std::string msg;
        {
            std::lock_guard<std::mutex> lk(mu_);
            msg = message_;
        }

        std::string line = "\r" + skin_.colors.banner_accent +
                           left + " " + face + " " + right +
                           skin_.colors.banner_text + " " + verb;
        if (!msg.empty()) line += ": " + msg;
        line += "\033[K";
        std::cerr << line << std::flush;

        ++idx;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

std::string build_tool_preview(const std::string& tool_name,
                               const nlohmann::json& args,
                               const std::string& result,
                               const SkinConfig& skin) {
    std::string emoji = get_tool_emoji(tool_name);
    std::string preview = skin.tool_prefix + " " + emoji + " " + tool_name;
    if (!args.empty()) {
        std::string args_str = args.dump();
        if (args_str.size() > 80) {
            args_str = args_str.substr(0, 77) + "...";
        }
        preview += " " + args_str;
    }
    if (!result.empty()) {
        std::string r = result;
        if (r.size() > 120) r = r.substr(0, 117) + "...";
        preview += "\n" + skin.tool_prefix + "   -> " + r;
    }
    return preview;
}

std::string get_tool_emoji(const std::string& tool_name) {
    if (tool_name == "bash" || tool_name == "execute_command")
        return "\xf0\x9f\x94\xa7";  // wrench
    if (tool_name == "read_file" || tool_name == "read")
        return "\xf0\x9f\x93\x84";  // page facing up
    if (tool_name == "write_file" || tool_name == "write")
        return "\xe2\x9c\x8f";      // pencil
    if (tool_name == "search" || tool_name == "grep")
        return "\xf0\x9f\x94\x8d";  // magnifying glass
    if (tool_name == "browser" || tool_name == "web")
        return "\xf0\x9f\x8c\x90";  // globe
    if (tool_name == "memory")
        return "\xf0\x9f\xa7\xa0";  // brain
    if (tool_name == "todo")
        return "\xe2\x9c\x85";      // check mark
    // Default.
    return "\xe2\x9a\x99";          // gear
}

}  // namespace hermes::cli
