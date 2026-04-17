// McpSession + SessionStore — HTTP/SSE session tracking for the MCP server.
//
// Each connected SSE client is assigned a UUID-v4 session id returned in the
// Mcp-Session-Id response header. The ``POST /messages`` endpoint takes a
// ``sessionId`` query parameter and routes the JSON-RPC payload to the
// correct session's SSE push queue.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::mcp_server {

// A bounded, thread-safe queue of SSE frames pending delivery to a client.
// Frames are already fully formatted ``event: message\ndata: ...\n\n``
// strings so the HTTP layer can just write them. Pushers wake waiters
// through a condition variable; ``wait_and_pop`` returns false on shutdown.
class SseQueue {
public:
    explicit SseQueue(std::size_t max_pending = 1024);

    // Push a pre-formatted SSE frame. Drops the oldest frame when full so
    // a slow reader cannot cause unbounded memory growth on the server.
    void push(std::string frame);

    // Block up to ``timeout`` for a frame. Returns empty optional on
    // timeout; false on shutdown requested via ``shutdown()``.
    bool wait_and_pop(std::string& out, std::chrono::milliseconds timeout);

    // Wake all waiters and refuse further pushes.
    void shutdown();

    bool is_shutdown() const;
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> frames_;
    std::size_t max_pending_;
    bool shutdown_ = false;
};

struct McpSession {
    std::string id;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_seen;
    // SSE push queue shared with the ``GET /sse`` handler. Never null for
    // sessions created via ``SessionStore::create()``.
    std::shared_ptr<SseQueue> queue;
    // Populated after the client sends ``initialize`` — used purely for
    // introspection / logging.
    std::string client_name;
    std::string client_version;
    bool initialized = false;

    // Resource URIs the client has subscribed to via
    // ``resources/subscribe``. Mutations must hold ``sub_mu``. A dedicated
    // per-session mutex avoids contention against the session-store lock
    // for the common read path (``is_subscribed``) used by notification
    // broadcasting.
    mutable std::mutex sub_mu;
    std::unordered_set<std::string> subscriptions;
};

class SessionStore {
public:
    // ttl controls how long an idle session may survive between calls to
    // ``gc_expired()``. ``max_sessions`` caps total concurrent sessions;
    // once exceeded, the oldest expired session is evicted first.
    explicit SessionStore(std::chrono::minutes ttl = std::chrono::minutes(30),
                          std::size_t max_sessions = 1024);

    // Mint a new UUID-v4 session id. Thread-safe.
    std::shared_ptr<McpSession> create();

    // Fetch a session by id and refresh its ``last_seen`` timestamp.
    // Returns nullptr when the session does not exist or has expired.
    std::shared_ptr<McpSession> touch(std::string_view id);

    // Passive lookup: does NOT refresh ``last_seen``.
    std::shared_ptr<McpSession> get(std::string_view id) const;

    // Remove a session. Wakes its SSE waiter. Returns true if present.
    bool drop(std::string_view id);

    // Drop every session whose ``last_seen`` is older than ``ttl_``.
    // Returns the number of sessions evicted.
    std::size_t gc_expired();

    // Testing / introspection helpers.
    std::size_t size() const;
    std::vector<std::string> list_ids() const;

    void set_ttl(std::chrono::minutes ttl);
    std::chrono::minutes ttl() const;

private:
    static std::string mint_uuid_v4();

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<McpSession>> sessions_;
    std::chrono::minutes ttl_;
    std::size_t max_sessions_;
};

}  // namespace hermes::mcp_server
