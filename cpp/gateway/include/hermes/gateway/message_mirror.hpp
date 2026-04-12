// MessageMirror — rules for mirroring messages across platforms/channels.
#pragma once

#include <hermes/gateway/gateway_config.hpp>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hermes::gateway {

class MessageMirror {
public:
    struct MirrorRule {
        Platform source;
        std::string source_chat;
        Platform dest;
        std::string dest_chat;
    };

    void add_rule(MirrorRule rule);

    // Check if a message should be mirrored; return destination targets.
    std::vector<std::pair<Platform, std::string>> get_mirrors(
        Platform source, const std::string& chat_id) const;

private:
    std::vector<MirrorRule> rules_;
    mutable std::mutex mu_;
};

}  // namespace hermes::gateway
