// Send-message tool — target parsing + gateway integration.
#pragma once

#include <string>
#include <string_view>

namespace hermes::gateway {
class GatewayRunner;
}

namespace hermes::tools {

struct ParsedTarget {
    std::string platform;
    std::string chat_id;
    std::string thread_id;
};

// Parse "platform:chat_id:thread_id".  Throws std::invalid_argument on
// malformed input.
ParsedTarget parse_target(std::string_view target);

/// Set the global gateway runner for send_message.  When set, the tool
/// can send messages through connected platform adapters.  When nullptr
/// (the default, e.g. in CLI mode), the tool returns a clear error.
void set_gateway_runner(hermes::gateway::GatewayRunner* runner);

void register_send_message_tools();

}  // namespace hermes::tools
