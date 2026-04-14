// Send-message tool — cross-channel messaging via platform APIs.
//
// Port of tools/send_message_tool.py.  This layer owns:
//   - target parsing (platform:chat_id:thread_id plus per-platform forms)
//   - secret redaction in error text
//   - media-attachment detection and mirror-text synthesis
//   - cron duplicate-send skip detection
//   - the send_message tool entry point (dispatched through a registered
//     gateway runner).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::gateway {
class GatewayRunner;
}

namespace hermes::tools {

// Per-platform canonical names recognised by the tool.
extern const std::vector<std::string>& supported_platforms();

struct ParsedTarget {
    std::string platform;
    std::string chat_id;
    std::string thread_id;
};

// Parse "platform:chat_id[:thread_id]".  Throws std::invalid_argument
// when the string does not contain at least one ':' separator.  Mostly
// kept for existing callers; new code should prefer parse_target_ref.
ParsedTarget parse_target(std::string_view target);

struct TargetRefResult {
    std::optional<std::string> chat_id;
    std::optional<std::string> thread_id;
    bool is_explicit = false;
};

// Parse the right-hand side of "platform:<ref>" for a specific platform.
// Returns is_explicit=true when the ref matches a structural pattern
// (numeric ID, Telegram topic id:thread, Feishu oc_/ou_, Discord
// snowflake:thread, WeChat wxid_/chatroom).  Non-structural refs (e.g.
// "#general") are returned with is_explicit=false so callers know to
// resolve via the channel directory.
TargetRefResult parse_target_ref(std::string_view platform,
                                 std::string_view ref);

// Redact tokens / API keys from arbitrary text before surfacing to
// users or model output.  Matches the behavior of
// agent.redact.redact_sensitive_text + URL query/key-value scrubbing.
std::string sanitize_error_text(std::string_view text);

struct MediaFile {
    std::string path;
    bool is_voice = false;
};

// Produce a human-readable mirror-text summary when the outgoing
// message contains only media attachments.
std::string describe_media_for_mirror(const std::vector<MediaFile>& media);

// Extract [media:/path/to/file] directives (and the legacy
// [voice:/path] form) from a message body.  Returns the cleaned body
// (directives removed) and the list of detected media files.
std::pair<std::string, std::vector<MediaFile>>
extract_media_directives(std::string_view message);

struct CronAutoDeliverTarget {
    std::string platform;
    std::string chat_id;
    std::optional<std::string> thread_id;
};

// Read HERMES_CRON_AUTO_DELIVER_* env vars.  Returns nullopt when
// any required var is missing.
std::optional<CronAutoDeliverTarget> get_cron_auto_delivery_target();

// If there is a cron auto-deliver target equal to the provided
// (platform, chat_id, thread_id), returns a JSON payload describing
// the skip.  Returns an empty string when no skip applies.
std::string maybe_skip_cron_duplicate_send(
    std::string_view platform, std::string_view chat_id,
    std::optional<std::string_view> thread_id);

// Gateway integration — CLI mode leaves this null and the tool reports
// a clear error; gateway mode installs a runner to actually dispatch.
void set_gateway_runner(hermes::gateway::GatewayRunner* runner);

using PlatformListFn =
    std::function<std::vector<std::pair<std::string, bool>>()>;

void set_platform_list_fn(PlatformListFn fn);

// Send-callback shape used by the gateway to perform an outbound send
// through the platform adapter.  Returns a JSON string result (the
// adapter's reply).  When null, the tool emits a tool_error.
using PlatformSendFn = std::function<std::string(
    const std::string& platform, const std::string& chat_id,
    const std::string& message,
    const std::optional<std::string>& thread_id,
    const std::vector<MediaFile>& media)>;

void set_platform_send_fn(PlatformSendFn fn);

void register_send_message_tools();

}  // namespace hermes::tools
