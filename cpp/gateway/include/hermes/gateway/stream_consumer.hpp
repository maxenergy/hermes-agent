// StreamConsumer — batches streaming LLM tokens into periodic edit callbacks.
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hermes::gateway {

class StreamConsumer {
public:
    using EditCallback = std::function<void(const std::string& chat_id,
                                            const std::string& message_id,
                                            const std::string& new_content)>;

    explicit StreamConsumer(EditCallback edit_cb);

    // Feed a single token from the streaming LLM response.
    // Batches updates and fires edit_cb at most every 500ms per chat_id:msg_id.
    void feed_token(const std::string& chat_id,
                    const std::string& message_id,
                    const std::string& token);

    // Force-flush accumulated tokens for a specific chat_id:message_id pair.
    void flush(const std::string& chat_id, const std::string& message_id);

    // Flush all pending buffers.
    void flush_all();

private:
    EditCallback edit_cb_;
    std::mutex mu_;
    // key = chat_id + ":" + message_id
    std::unordered_map<std::string, std::string> buffers_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_edit_;

    static constexpr auto BATCH_INTERVAL = std::chrono::milliseconds(500);

    std::string make_key(const std::string& chat_id,
                         const std::string& message_id) const;
    void do_flush(const std::string& key, const std::string& chat_id,
                  const std::string& message_id);
};

}  // namespace hermes::gateway
