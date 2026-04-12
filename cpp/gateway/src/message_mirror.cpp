#include <hermes/gateway/message_mirror.hpp>

namespace hermes::gateway {

void MessageMirror::add_rule(MirrorRule rule) {
    std::lock_guard<std::mutex> lock(mu_);
    rules_.push_back(std::move(rule));
}

std::vector<std::pair<Platform, std::string>>
MessageMirror::get_mirrors(Platform source,
                           const std::string& chat_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<Platform, std::string>> result;
    for (const auto& rule : rules_) {
        if (rule.source == source && rule.source_chat == chat_id) {
            result.emplace_back(rule.dest, rule.dest_chat);
        }
    }
    return result;
}

}  // namespace hermes::gateway
