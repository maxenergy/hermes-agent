// Phase 12 — DingTalk platform adapter implementation.
#include "dingtalk.hpp"

namespace hermes::gateway::platforms {

DingTalkAdapter::DingTalkAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool DingTalkAdapter::connect() {
    if (cfg_.client_id.empty() || cfg_.client_secret.empty()) return false;
    // TODO(phase-14+): obtain access_token via OAuth.
    return true;
}

void DingTalkAdapter::disconnect() {}

bool DingTalkAdapter::send(const std::string& /*chat_id*/,
                           const std::string& /*content*/) {
    return true;
}

void DingTalkAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string DingTalkAdapter::extract_mention(const std::string& text) {
    // Stub: look for @mention pattern. Real impl would use atMobiles/atUserIds.
    auto pos = text.find('@');
    if (pos == std::string::npos) return {};
    auto end = text.find(' ', pos);
    if (end == std::string::npos) end = text.size();
    return text.substr(pos + 1, end - pos - 1);
}

}  // namespace hermes::gateway::platforms
