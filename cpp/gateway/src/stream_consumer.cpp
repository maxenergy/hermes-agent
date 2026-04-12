#include <hermes/gateway/stream_consumer.hpp>

namespace hermes::gateway {

StreamConsumer::StreamConsumer(EditCallback edit_cb)
    : edit_cb_(std::move(edit_cb)) {}

std::string StreamConsumer::make_key(const std::string& chat_id,
                                     const std::string& message_id) const {
    return chat_id + ":" + message_id;
}

void StreamConsumer::do_flush(const std::string& key,
                              const std::string& chat_id,
                              const std::string& message_id) {
    // Caller must hold mu_.
    auto it = buffers_.find(key);
    if (it == buffers_.end() || it->second.empty()) return;

    if (edit_cb_) {
        edit_cb_(chat_id, message_id, it->second);
    }
    last_edit_[key] = std::chrono::steady_clock::now();
    // Do NOT clear the buffer — accumulated content represents the full
    // message so far (each edit replaces the whole message).
}

void StreamConsumer::feed_token(const std::string& chat_id,
                                const std::string& message_id,
                                const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    auto key = make_key(chat_id, message_id);
    buffers_[key] += token;

    auto now = std::chrono::steady_clock::now();
    auto it = last_edit_.find(key);
    if (it == last_edit_.end() || (now - it->second) >= BATCH_INTERVAL) {
        do_flush(key, chat_id, message_id);
    }
}

void StreamConsumer::flush(const std::string& chat_id,
                           const std::string& message_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto key = make_key(chat_id, message_id);
    do_flush(key, chat_id, message_id);
    // After explicit flush, remove the buffer entry.
    buffers_.erase(key);
    last_edit_.erase(key);
}

void StreamConsumer::flush_all() {
    std::lock_guard<std::mutex> lock(mu_);
    // Collect keys first since do_flush doesn't erase.
    std::vector<std::string> keys;
    keys.reserve(buffers_.size());
    for (const auto& [k, _] : buffers_) {
        keys.push_back(k);
    }
    for (const auto& key : keys) {
        // Parse chat_id and message_id from the key.
        auto colon = key.find(':');
        if (colon != std::string::npos) {
            auto cid = key.substr(0, colon);
            auto mid = key.substr(colon + 1);
            do_flush(key, cid, mid);
        }
    }
    buffers_.clear();
    last_edit_.clear();
}

}  // namespace hermes::gateway
