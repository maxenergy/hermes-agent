// SessionDB — schema v6 SQLite store with FTS5 full-text search.
//
// A single SessionDB instance owns one sqlite3 connection and serializes
// access behind an internal mutex. The connection is opened in WAL mode
// with synchronous=NORMAL and a 5s busy_timeout. Every write path also
// retries up to 15 times on SQLITE_BUSY using hermes::core::retry's
// jittered backoff — this is belt-and-suspenders protection against
// cross-process contention that busy_timeout alone can miss.
//
// For cross-process concurrency: open a SessionDB per writer. SQLite's
// WAL journal + file locks coordinate between processes.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::state {

struct SessionRow {
    std::string id;
    std::string source;
    std::string model;
    nlohmann::json config;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    double cost_usd = 0.0;
    std::optional<std::string> title;
};

struct MessageRow {
    std::int64_t id = 0;
    std::string session_id;
    int turn_index = 0;
    std::string role;
    std::string content;
    nlohmann::json tool_calls;
    std::optional<std::string> reasoning;
    std::chrono::system_clock::time_point created_at;
};

struct FtsHit {
    std::string session_id;
    std::int64_t message_id = 0;
    std::string snippet;
    double score = 0.0;
};

class SessionDB {
public:
    // Opens (or creates) the database at get_hermes_home() / "sessions.db".
    SessionDB();
    explicit SessionDB(const std::filesystem::path& path);
    ~SessionDB();

    SessionDB(const SessionDB&) = delete;
    SessionDB& operator=(const SessionDB&) = delete;
    SessionDB(SessionDB&&) noexcept;
    SessionDB& operator=(SessionDB&&) noexcept;

    // ----- Sessions -----
    std::string create_session(const std::string& source,
                               const std::string& model,
                               const nlohmann::json& config);
    std::optional<SessionRow> get_session(const std::string& id);
    std::vector<SessionRow> list_sessions(int limit = 50, int offset = 0);
    void update_session_title(const std::string& id, const std::string& title);
    void add_tokens(const std::string& id,
                    std::int64_t input,
                    std::int64_t output,
                    double cost);
    void delete_session(const std::string& id);

    // ----- Messages -----
    std::int64_t save_message(const MessageRow& msg);
    std::vector<MessageRow> get_messages(const std::string& session_id);

    // ----- FTS5 -----
    std::vector<FtsHit> fts_search(const std::string& query, int limit = 20);

    // ----- Maintenance -----
    // Force a PASSIVE WAL checkpoint. Normally called automatically every
    // 50 writes; exposed for tests and for explicit flush-on-shutdown.
    void checkpoint();
    int schema_version();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hermes::state
