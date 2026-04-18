// Image attachment helpers for the interactive REPL.
//
// `HermesCLI` queues image attachments in `pending_image_path_` from the
// `/paste` and `/image` slash commands.  When the next plain-text message is
// dispatched to the agent, we read the file, base64-encode the bytes, and
// emit a multimodal user `Message` with two content blocks: a `text` block
// carrying the user's prompt and an `image_url` block carrying a `data:<mime>;
// base64,<payload>` URL compatible with both OpenAI and Anthropic (our
// anthropic_adapter_depth layer converts `image_url` → `image/source/base64`).
//
// The helper is intentionally UI-free so it can be unit-tested in isolation.
#pragma once

#include "hermes/llm/message.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace hermes::cli {

// Hard cap to avoid OOM'ing the process (and the LLM request body) on a
// stray `/image` pointing at a multi-gigabyte file.  20 MB matches the
// limit used by the Python reference implementation.
inline constexpr std::size_t kMaxImageAttachmentBytes = 20 * 1024 * 1024;

// Reason codes surfaced by `build_image_user_message` when the helper
// refuses to attach an image.  Translated to a user-facing string by the
// caller.
enum class AttachmentError {
    Ok,           // no error; message populated
    EmptyPath,    // path is empty — text-only
    NotFound,     // file does not exist or is not readable
    TooLarge,     // file exceeds kMaxImageAttachmentBytes
    ReadFailed,   // file exists but reading failed (permissions, IO)
};

struct AttachmentResult {
    AttachmentError error{AttachmentError::Ok};
    // Populated only when error == Ok.  Role is User, content_blocks holds a
    // {type:"text"} followed by a {type:"image_url"} block.
    hermes::llm::Message message;
    // When error != Ok, a short human-readable reason.  The caller is
    // expected to prepend context (the path) before printing.
    std::string detail;
};

// Infer the image MIME type from a filename's extension.  Unknown
// extensions fall back to "image/png" — both OpenAI and Anthropic accept
// data URLs with a mismatched media_type as long as the payload is valid
// base64.
std::string infer_image_mime_from_path(std::string_view path);

// Build a multimodal user turn that combines `text` with the image at
// `image_path`.  When `image_path` is empty, returns `EmptyPath` and an
// empty Message — callers should then fall through to the text-only path.
AttachmentResult build_image_user_message(const std::string& text,
                                          const std::string& image_path);

}  // namespace hermes::cli
