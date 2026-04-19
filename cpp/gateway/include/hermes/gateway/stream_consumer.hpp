// StreamConsumer — batches streaming LLM tokens into periodic edit callbacks.
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hermes::gateway {

// Capability flag ported from gateway/platforms/base.py's
// ``REQUIRES_EDIT_FINALIZE`` class attribute (upstream 4459913f).
// Default false: most platforms (Telegram / Slack / Discord / Matrix)
// treat ``finalize=true`` as a no-op and skip the redundant final edit.
// Adapters whose streaming UI has an "in progress" state that must be
// explicitly closed (e.g. DingTalk AI Cards) set this to true so the
// stream consumer routes a final edit even when the content is unchanged.
struct StreamConsumerAdapterCapabilities {
    bool requires_edit_finalize = false;
};

class StreamConsumer {
public:
    using AdapterCapabilities = StreamConsumerAdapterCapabilities;

    // Edit callback now receives a ``finalize`` flag.  ``true`` signals
    // the last edit of the streaming sequence.  Adapters that don't
    // care about the flag simply ignore it.  Kept as a signature change
    // rather than an overload so downstream adapter code is forced to
    // acknowledge the flag when porting.
    using EditCallback = std::function<void(const std::string& chat_id,
                                            const std::string& message_id,
                                            const std::string& new_content,
                                            bool finalize)>;

    explicit StreamConsumer(EditCallback edit_cb,
                              AdapterCapabilities caps = {});

    // Override the adapter capability flag at runtime — used by
    // adapters that only discover their streaming mode after connect.
    void set_adapter_capabilities(AdapterCapabilities caps);
    AdapterCapabilities adapter_capabilities() const;
    bool requires_edit_finalize() const;

    // Feed a single token from the streaming LLM response.
    // Batches updates and fires edit_cb at most every 500ms per chat_id:msg_id.
    void feed_token(const std::string& chat_id,
                    const std::string& message_id,
                    const std::string& token);

    // Force-flush accumulated tokens for a specific chat_id:message_id pair.
    // ``finalize`` propagates into the edit callback: true means "this is
    // the last edit".  For REQUIRES_EDIT_FINALIZE adapters the callback
    // runs even when the content is identical to the last edit so the
    // streaming UI can transition out of the in-progress state.
    void flush(const std::string& chat_id, const std::string& message_id,
               bool finalize = false);

    // Flush all pending buffers.  ``finalize`` applies to every chat
    // being flushed — used at stream-end / shutdown.
    void flush_all(bool finalize = false);

private:
    EditCallback edit_cb_;
    AdapterCapabilities caps_;
    mutable std::mutex mu_;
    // key = chat_id + ":" + message_id
    std::unordered_map<std::string, std::string> buffers_;
    std::unordered_map<std::string, std::string> last_sent_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_edit_;

    static constexpr auto BATCH_INTERVAL = std::chrono::milliseconds(500);

    std::string make_key(const std::string& chat_id,
                         const std::string& message_id) const;
    void do_flush(const std::string& key, const std::string& chat_id,
                  const std::string& message_id, bool finalize);
};

}  // namespace hermes::gateway
