#include <hermes/gateway/stream_consumer.hpp>

#include <vector>

namespace hermes::gateway {

StreamConsumer::StreamConsumer(EditCallback edit_cb,
                                 AdapterCapabilities caps)
    : edit_cb_(std::move(edit_cb)), caps_(caps) {}

void StreamConsumer::set_adapter_capabilities(AdapterCapabilities caps) {
    std::lock_guard<std::mutex> lock(mu_);
    caps_ = caps;
}

StreamConsumer::AdapterCapabilities
StreamConsumer::adapter_capabilities() const {
    std::lock_guard<std::mutex> lock(mu_);
    return caps_;
}

bool StreamConsumer::requires_edit_finalize() const {
    std::lock_guard<std::mutex> lock(mu_);
    return caps_.requires_edit_finalize;
}

std::string StreamConsumer::make_key(const std::string& chat_id,
                                     const std::string& message_id) const {
    return chat_id + ":" + message_id;
}

void StreamConsumer::do_flush(const std::string& key,
                              const std::string& chat_id,
                              const std::string& message_id,
                              bool finalize) {
    // Caller must hold mu_.
    auto it = buffers_.find(key);
    if (it == buffers_.end()) return;
    const std::string& content = it->second;

    // Skip identical content.  Exception: when the adapter declares
    // ``REQUIRES_EDIT_FINALIZE`` and this is a finalize call, we still
    // fire the edit so the streaming UI transitions out of the
    // in-progress state even if the text didn't change (mirrors
    // gateway/stream_consumer.py:_send_or_edit short-circuit).
    auto last_it = last_sent_.find(key);
    bool identical = last_it != last_sent_.end() && last_it->second == content;
    bool force_for_finalize = finalize && caps_.requires_edit_finalize;
    if (identical && !force_for_finalize) {
        last_edit_[key] = std::chrono::steady_clock::now();
        return;
    }
    if (content.empty() && !force_for_finalize) return;

    if (edit_cb_) {
        edit_cb_(chat_id, message_id, content, finalize);
    }
    last_sent_[key] = content;
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
        do_flush(key, chat_id, message_id, /*finalize=*/false);
    }
}

void StreamConsumer::flush(const std::string& chat_id,
                           const std::string& message_id,
                           bool finalize) {
    std::lock_guard<std::mutex> lock(mu_);
    auto key = make_key(chat_id, message_id);
    do_flush(key, chat_id, message_id, finalize);
    // After explicit flush, remove the buffer entry.
    buffers_.erase(key);
    last_edit_.erase(key);
    last_sent_.erase(key);
}

void StreamConsumer::flush_all(bool finalize) {
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
            do_flush(key, cid, mid, finalize);
        }
    }
    buffers_.clear();
    last_edit_.clear();
    last_sent_.clear();
}

}  // namespace hermes::gateway
