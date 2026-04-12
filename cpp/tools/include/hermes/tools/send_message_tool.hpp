// Phase 8: Send-message tool — target parsing + gateway stub.
#pragma once

#include <string>
#include <string_view>

namespace hermes::tools {

struct ParsedTarget {
    std::string platform;
    std::string chat_id;
    std::string thread_id;
};

// Parse "platform:chat_id:thread_id".  Throws std::invalid_argument on
// malformed input.
ParsedTarget parse_target(std::string_view target);

void register_send_message_tools();

}  // namespace hermes::tools
