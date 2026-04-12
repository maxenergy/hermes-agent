#include <hermes/cron/delivery.hpp>
#include <hermes/core/atomic_io.hpp>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace hermes::cron {

DeliveryTarget parse_delivery_target(std::string_view target_str) {
    DeliveryTarget dt;

    if (target_str == "origin") {
        dt.platform = "origin";
        dt.is_origin = true;
        return dt;
    }

    if (target_str == "local") {
        dt.platform = "local";
        dt.is_explicit = true;
        return dt;
    }

    // Parse "platform" or "platform:chat_id" or "platform:chat_id:thread_id"
    auto first_colon = target_str.find(':');
    if (first_colon == std::string_view::npos) {
        dt.platform = std::string(target_str);
        dt.is_explicit = true;
        return dt;
    }

    dt.platform = std::string(target_str.substr(0, first_colon));
    dt.is_explicit = true;
    auto rest = target_str.substr(first_colon + 1);

    auto second_colon = rest.find(':');
    if (second_colon == std::string_view::npos) {
        dt.chat_id = std::string(rest);
    } else {
        dt.chat_id = std::string(rest.substr(0, second_colon));
        dt.thread_id = std::string(rest.substr(second_colon + 1));
    }
    return dt;
}

void DeliveryRouter::set_gateway_sender(PlatformSendFn sender) {
    gateway_sender_ = std::move(sender);
}

void DeliveryRouter::deliver(const std::string& content,
                              const std::vector<DeliveryTarget>& targets,
                              const std::string& job_id,
                              const std::string& run_id) {
    for (const auto& target : targets) {
        if (target.platform == "local") {
            deliver_local(content, job_id, run_id);
        } else {
            deliver_to_platform(content, target);
        }
    }
}

void DeliveryRouter::deliver_to_platform(const std::string& content,
                                          const DeliveryTarget& target) {
    if (!gateway_sender_) {
        throw std::runtime_error(
            "gateway not running — cannot deliver to " + target.platform);
    }
    gateway_sender_(target.platform, target.chat_id, content);
}

void DeliveryRouter::deliver_local(const std::string& content,
                                    const std::string& job_id,
                                    const std::string& run_id) {
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    auto dir = fs::path(home) / ".hermes" / "cron" / "output" / job_id;
    fs::create_directories(dir);
    auto file = dir / (run_id + ".txt");
    hermes::core::atomic_io::atomic_write(file, content);
}

}  // namespace hermes::cron
