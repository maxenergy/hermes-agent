#include <hermes/gateway/message_pipeline.hpp>

#include <algorithm>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <hermes/gateway/gateway_helpers.hpp>

namespace hermes::gateway {

// --- Queue ---------------------------------------------------------------

bool PendingQueue::enqueue(const std::string& session_key,
                             MessageEvent event) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& q = queues_[session_key];
    bool merged = false;
    if (!q.empty()) {
        q.back() = merge_pending_message_event(q.back(), event);
        merged = true;
    } else {
        q.push_back(std::move(event));
    }
    return merged;
}

std::optional<MessageEvent> PendingQueue::dequeue(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end() || it->second.empty()) return std::nullopt;
    auto ev = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) queues_.erase(it);
    return ev;
}

std::optional<std::string> PendingQueue::dequeue_text(
    const std::string& session_key) {
    auto ev = dequeue(session_key);
    if (!ev) return std::nullopt;
    if (!ev->text.empty()) return ev->text;
    if (!ev->media_urls.empty()) {
        MediaPlaceholderEvent mp;
        mp.message_type = ev->message_type;
        mp.media_urls = ev->media_urls;
        return build_media_placeholder(mp);
    }
    return std::string{};
}

std::optional<MessageEvent> PendingQueue::peek(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end() || it->second.empty()) return std::nullopt;
    return it->second.front();
}

void PendingQueue::clear(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    queues_.erase(session_key);
}

std::size_t PendingQueue::session_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queues_.size();
}

// --- merge_pending_message_event ----------------------------------------

MessageEvent merge_pending_message_event(const MessageEvent& existing,
                                          const MessageEvent& incoming) {
    MessageEvent out = existing;

    // Later text wins (Python behavior).  Preserve existing text only
    // when incoming is empty and existing has content.
    if (!incoming.text.empty()) {
        out.text = incoming.text;
    }

    // Message type: prefer the richer of the two (photo > text).
    if (!incoming.message_type.empty() && incoming.message_type != "TEXT") {
        out.message_type = incoming.message_type;
    } else if (out.message_type.empty()) {
        out.message_type = incoming.message_type;
    }

    // Stack + dedupe media urls.
    std::unordered_set<std::string> seen(out.media_urls.begin(),
                                          out.media_urls.end());
    for (const auto& u : incoming.media_urls) {
        if (!seen.count(u)) {
            out.media_urls.push_back(u);
            seen.insert(u);
        }
    }

    // Incoming reply_to overrides if set.
    if (incoming.reply_to_message_id.has_value()) {
        out.reply_to_message_id = incoming.reply_to_message_id;
    }

    // Source: keep existing unless fields are clearly missing.
    if (out.source.chat_id.empty()) out.source = incoming.source;
    return out;
}

// --- MessagePipeline -----------------------------------------------------

MessagePipeline::MessagePipeline(PendingQueue* queue, bool queue_during_drain)
    : queue_(queue), queue_during_drain_(queue_during_drain) {}

BusyResult MessagePipeline::evaluate_busy(const MessageEvent& event,
                                           const std::string& session_key,
                                           bool is_draining,
                                           std::string_view action_gerund) {
    BusyResult res;
    if (!is_draining) {
        // Not draining — simply queue for the busy-but-live case; the
        // caller (GatewayRunner) still needs to emit the normal "agent
        // busy" message for that path.  We just queue here.
        if (queue_) queue_->enqueue(session_key, event);
        res.decision = BusyDecision::Queued;
        res.notice = "";
        return res;
    }

    if (queue_during_drain_) {
        if (queue_) queue_->enqueue(session_key, event);
        std::ostringstream os;
        os << "\xE2\x8F\xB3 Gateway " << action_gerund
           << " \xE2\x80\x94 queued for the next turn after it comes back.";
        res.decision = BusyDecision::Queued;
        res.notice = os.str();
        return res;
    }

    std::ostringstream os;
    os << "\xE2\x8F\xB3 Gateway is " << action_gerund
       << " and is not accepting another turn right now.";
    res.decision = BusyDecision::Rejected;
    res.notice = os.str();
    return res;
}

MessageEvent MessagePipeline::synthesize_watch_notification(
    const std::string& text, const SessionSource& source) {
    MessageEvent ev;
    ev.text = text;
    ev.message_type = "TEXT";
    ev.source = source;
    return ev;
}

MessagePipeline::DrainStats MessagePipeline::drain_active(
    CountSampler sampler, StatusCallback status,
    std::chrono::milliseconds timeout) {
    DrainStats st;
    st.initial_count = sampler ? sampler() : 0;
    if (status) status(st.initial_count, true);

    if (st.initial_count == 0 || timeout.count() <= 0) {
        st.final_count = st.initial_count;
        st.timed_out = st.initial_count > 0;
        if (status) status(st.final_count, true);
        return st;
    }

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + timeout;
    int last_count = st.initial_count;
    auto last_status = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        int cur = sampler();
        auto now = std::chrono::steady_clock::now();
        if (cur != last_count ||
            (now - last_status) >= std::chrono::seconds(1)) {
            if (status) status(cur, false);
            last_count = cur;
            last_status = now;
        }
        if (cur == 0) {
            st.final_count = 0;
            st.timed_out = false;
            st.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start);
            if (status) status(0, true);
            return st;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    st.final_count = sampler();
    st.timed_out = st.final_count > 0;
    st.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (status) status(st.final_count, true);
    return st;
}

}  // namespace hermes::gateway
