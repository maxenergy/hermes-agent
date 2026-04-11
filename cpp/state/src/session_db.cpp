#include "hermes/state/session_db.hpp"

#include "hermes/core/path.hpp"
#include "hermes/core/retry.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hermes::state {

namespace {

constexpr int kTargetSchemaVersion = 6;
constexpr int kMaxBusyRetries = 15;
constexpr int kCheckpointWriteThreshold = 50;

std::int64_t now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point tp_from_millis(std::int64_t ms) {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{ms}};
}

// UUIDv4-ish string — purely random, good enough for primary keys.
std::string make_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t hi = dist(rng);
    std::uint64_t lo = dist(rng);

    // Force variant (10xxxxxx) and version (0100) bits to match UUIDv4.
    hi = (hi & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

    auto to_hex = [](std::uint64_t v, int nibbles) {
        std::string out(static_cast<std::size_t>(nibbles), '0');
        for (int i = nibbles - 1; i >= 0; --i) {
            int d = static_cast<int>(v & 0xf);
            out[static_cast<std::size_t>(i)] =
                static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
            v >>= 4;
        }
        return out;
    };

    std::string hi_hex = to_hex(hi, 16);
    std::string lo_hex = to_hex(lo, 16);
    std::ostringstream oss;
    oss << hi_hex.substr(0, 8) << '-' << hi_hex.substr(8, 4) << '-'
        << hi_hex.substr(12, 4) << '-' << lo_hex.substr(0, 4) << '-'
        << lo_hex.substr(4, 12);
    return oss.str();
}

// Small helper — sleep for the jittered backoff delay associated with
// a given retry attempt.
void backoff_sleep(int attempt) {
    auto delay = hermes::core::retry::jittered_backoff(
        attempt,
        std::chrono::milliseconds(20),
        std::chrono::milliseconds(150),
        0.5);
    std::this_thread::sleep_for(delay);
}

}  // namespace

struct SessionDB::Impl {
    std::filesystem::path path;
    std::unique_ptr<SQLite::Database> db;
    std::mutex mtx;
    int writes_since_checkpoint = 0;

    explicit Impl(std::filesystem::path p) : path(std::move(p)) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        (void)ec;

        db = std::make_unique<SQLite::Database>(
            path.string(),
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
            5000  // busy_timeout in milliseconds
        );

        db->exec("PRAGMA journal_mode=WAL;");
        db->exec("PRAGMA synchronous=NORMAL;");
        db->exec("PRAGMA busy_timeout=5000;");
        db->exec("PRAGMA foreign_keys=ON;");

        require_fts5();
        run_migrations();
    }

    void require_fts5() {
        // Probe: create a temporary FTS5 table. If SQLite wasn't built
        // with FTS5 this raises a clear error instead of the cryptic
        // "no such module: fts5" later when we try to read.
        try {
            db->exec(
                "CREATE VIRTUAL TABLE IF NOT EXISTS _hermes_fts5_probe "
                "USING fts5(x);");
            db->exec("DROP TABLE IF EXISTS _hermes_fts5_probe;");
        } catch (const SQLite::Exception& e) {
            throw std::runtime_error(
                std::string("SQLite build is missing FTS5 support: ") +
                e.what());
        }
    }

    int read_schema_version() {
        try {
            SQLite::Statement q(*db,
                                "SELECT version FROM schema_version LIMIT 1");
            if (q.executeStep()) {
                return q.getColumn(0).getInt();
            }
        } catch (const SQLite::Exception&) {
            // Table doesn't exist yet — fresh install.
        }
        return 0;
    }

    void run_migrations() {
        db->exec(
            "CREATE TABLE IF NOT EXISTS schema_version ("
            "  version INTEGER PRIMARY KEY);");

        int current = read_schema_version();
        if (current == kTargetSchemaVersion) {
            return;
        }

        SQLite::Transaction tx(*db);

        // Phase 2 ships a single-shot v6 install — older on-disk
        // schemas are not part of the C++17 port's requirements, so
        // any non-zero version that disagrees with v6 is an error we
        // surface clearly to the caller.
        if (current != 0 && current != kTargetSchemaVersion) {
            throw std::runtime_error(
                "SessionDB: unsupported on-disk schema version " +
                std::to_string(current) +
                " (expected " + std::to_string(kTargetSchemaVersion) + ")");
        }

        db->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS sessions (
              id TEXT PRIMARY KEY,
              source TEXT NOT NULL,
              model TEXT,
              config TEXT,
              created_at INTEGER NOT NULL,
              updated_at INTEGER NOT NULL,
              input_tokens INTEGER NOT NULL DEFAULT 0,
              output_tokens INTEGER NOT NULL DEFAULT 0,
              cost_usd REAL NOT NULL DEFAULT 0.0,
              title TEXT
            );
        )SQL");

        db->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS messages (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
              turn_index INTEGER NOT NULL,
              role TEXT NOT NULL,
              content TEXT,
              tool_calls TEXT,
              reasoning TEXT,
              created_at INTEGER NOT NULL
            );
        )SQL");

        db->exec(
            "CREATE INDEX IF NOT EXISTS idx_messages_session "
            "ON messages(session_id, turn_index);");

        db->exec(R"SQL(
            CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
              content,
              session_id UNINDEXED,
              role UNINDEXED,
              content='messages',
              content_rowid='id'
            );
        )SQL");

        db->exec(R"SQL(
            CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
              INSERT INTO messages_fts(rowid, content, session_id, role)
              VALUES (new.id, new.content, new.session_id, new.role);
            END;
        )SQL");

        db->exec(R"SQL(
            CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
              INSERT INTO messages_fts(messages_fts, rowid, content, session_id, role)
              VALUES('delete', old.id, old.content, old.session_id, old.role);
            END;
        )SQL");

        db->exec(R"SQL(
            CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
              INSERT INTO messages_fts(messages_fts, rowid, content, session_id, role)
              VALUES('delete', old.id, old.content, old.session_id, old.role);
              INSERT INTO messages_fts(rowid, content, session_id, role)
              VALUES (new.id, new.content, new.session_id, new.role);
            END;
        )SQL");

        db->exec("DELETE FROM schema_version;");
        SQLite::Statement ins(
            *db, "INSERT INTO schema_version(version) VALUES(?)");
        ins.bind(1, kTargetSchemaVersion);
        ins.exec();

        tx.commit();
    }

    // Execute `fn` inside a transaction with up to kMaxBusyRetries
    // retries on SQLITE_BUSY / SQLITE_LOCKED. Every successful call
    // increments the write counter and triggers a PASSIVE checkpoint
    // once the threshold is crossed.
    template <typename Fn>
    auto with_write_retry(Fn&& fn) -> decltype(fn()) {
        int attempt = 1;
        for (;;) {
            try {
                SQLite::Transaction tx(*db);
                auto result = fn();
                tx.commit();

                if (++writes_since_checkpoint >= kCheckpointWriteThreshold) {
                    writes_since_checkpoint = 0;
                    try {
                        db->exec("PRAGMA wal_checkpoint(PASSIVE);");
                    } catch (const SQLite::Exception&) {
                        // Best effort — a failed checkpoint is not fatal.
                    }
                }
                return result;
            } catch (const SQLite::Exception& e) {
                int code = e.getErrorCode();
                if ((code == SQLITE_BUSY || code == SQLITE_LOCKED) &&
                    attempt < kMaxBusyRetries) {
                    backoff_sleep(attempt);
                    ++attempt;
                    continue;
                }
                throw;
            }
        }
    }
};

// ---- Construction / destruction -----------------------------------------

SessionDB::SessionDB()
    : SessionDB(hermes::core::path::get_hermes_home() / "sessions.db") {}

SessionDB::SessionDB(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}

SessionDB::~SessionDB() = default;
SessionDB::SessionDB(SessionDB&&) noexcept = default;
SessionDB& SessionDB::operator=(SessionDB&&) noexcept = default;

// ---- Sessions -----------------------------------------------------------

std::string SessionDB::create_session(const std::string& source,
                                      const std::string& model,
                                      const nlohmann::json& config) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::string id = make_uuid();
    std::string config_str = config.is_null() ? std::string("{}") : config.dump();
    std::int64_t now_ms = now_millis();

    impl_->with_write_retry([&]() -> int {
        SQLite::Statement stmt(*impl_->db, R"SQL(
            INSERT INTO sessions
              (id, source, model, config, created_at, updated_at,
               input_tokens, output_tokens, cost_usd)
            VALUES (?, ?, ?, ?, ?, ?, 0, 0, 0.0)
        )SQL");
        stmt.bind(1, id);
        stmt.bind(2, source);
        stmt.bind(3, model);
        stmt.bind(4, config_str);
        stmt.bind(5, static_cast<int64_t>(now_ms));
        stmt.bind(6, static_cast<int64_t>(now_ms));
        stmt.exec();
        return 0;
    });

    return id;
}

std::optional<SessionRow> SessionDB::get_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SQLite::Statement q(*impl_->db, R"SQL(
        SELECT id, source, model, config, created_at, updated_at,
               input_tokens, output_tokens, cost_usd, title
          FROM sessions WHERE id = ?
    )SQL");
    q.bind(1, id);

    if (!q.executeStep()) {
        return std::nullopt;
    }

    SessionRow row;
    row.id = q.getColumn(0).getString();
    row.source = q.getColumn(1).getString();
    row.model = q.getColumn(2).isNull() ? std::string{}
                                         : q.getColumn(2).getString();
    std::string cfg = q.getColumn(3).isNull() ? std::string{}
                                               : q.getColumn(3).getString();
    row.config = cfg.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(cfg, nullptr, false);
    if (row.config.is_discarded()) {
        row.config = nlohmann::json::object();
    }
    row.created_at = tp_from_millis(q.getColumn(4).getInt64());
    row.updated_at = tp_from_millis(q.getColumn(5).getInt64());
    row.input_tokens = q.getColumn(6).getInt64();
    row.output_tokens = q.getColumn(7).getInt64();
    row.cost_usd = q.getColumn(8).getDouble();
    if (!q.getColumn(9).isNull()) {
        row.title = q.getColumn(9).getString();
    }
    return row;
}

std::vector<SessionRow> SessionDB::list_sessions(int limit, int offset) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SQLite::Statement q(*impl_->db, R"SQL(
        SELECT id, source, model, config, created_at, updated_at,
               input_tokens, output_tokens, cost_usd, title
          FROM sessions
         ORDER BY created_at DESC
         LIMIT ? OFFSET ?
    )SQL");
    q.bind(1, limit);
    q.bind(2, offset);

    std::vector<SessionRow> rows;
    while (q.executeStep()) {
        SessionRow row;
        row.id = q.getColumn(0).getString();
        row.source = q.getColumn(1).getString();
        row.model = q.getColumn(2).isNull() ? std::string{}
                                             : q.getColumn(2).getString();
        std::string cfg = q.getColumn(3).isNull() ? std::string{}
                                                   : q.getColumn(3).getString();
        row.config = cfg.empty() ? nlohmann::json::object()
                                  : nlohmann::json::parse(cfg, nullptr, false);
        if (row.config.is_discarded()) {
            row.config = nlohmann::json::object();
        }
        row.created_at = tp_from_millis(q.getColumn(4).getInt64());
        row.updated_at = tp_from_millis(q.getColumn(5).getInt64());
        row.input_tokens = q.getColumn(6).getInt64();
        row.output_tokens = q.getColumn(7).getInt64();
        row.cost_usd = q.getColumn(8).getDouble();
        if (!q.getColumn(9).isNull()) {
            row.title = q.getColumn(9).getString();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void SessionDB::update_session_title(const std::string& id,
                                     const std::string& title) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->with_write_retry([&]() -> int {
        SQLite::Statement stmt(
            *impl_->db,
            "UPDATE sessions SET title = ?, updated_at = ? WHERE id = ?");
        stmt.bind(1, title);
        stmt.bind(2, static_cast<int64_t>(now_millis()));
        stmt.bind(3, id);
        stmt.exec();
        return 0;
    });
}

void SessionDB::add_tokens(const std::string& id,
                           std::int64_t input,
                           std::int64_t output,
                           double cost) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->with_write_retry([&]() -> int {
        SQLite::Statement stmt(*impl_->db, R"SQL(
            UPDATE sessions
               SET input_tokens = input_tokens + ?,
                   output_tokens = output_tokens + ?,
                   cost_usd = cost_usd + ?,
                   updated_at = ?
             WHERE id = ?
        )SQL");
        stmt.bind(1, static_cast<int64_t>(input));
        stmt.bind(2, static_cast<int64_t>(output));
        stmt.bind(3, cost);
        stmt.bind(4, static_cast<int64_t>(now_millis()));
        stmt.bind(5, id);
        stmt.exec();
        return 0;
    });
}

void SessionDB::delete_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->with_write_retry([&]() -> int {
        // ON DELETE CASCADE on messages.session_id handles cleanup.
        SQLite::Statement stmt(*impl_->db,
                               "DELETE FROM sessions WHERE id = ?");
        stmt.bind(1, id);
        stmt.exec();
        return 0;
    });
}

// ---- Messages -----------------------------------------------------------

std::int64_t SessionDB::save_message(const MessageRow& msg) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::string tool_calls_str = msg.tool_calls.is_null()
                                     ? std::string{}
                                     : msg.tool_calls.dump();
    auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          msg.created_at.time_since_epoch())
                          .count();
    if (created_ms == 0) {
        created_ms = now_millis();
    }

    return impl_->with_write_retry([&]() -> std::int64_t {
        SQLite::Statement stmt(*impl_->db, R"SQL(
            INSERT INTO messages
              (session_id, turn_index, role, content, tool_calls,
               reasoning, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        )SQL");
        stmt.bind(1, msg.session_id);
        stmt.bind(2, msg.turn_index);
        stmt.bind(3, msg.role);
        stmt.bind(4, msg.content);
        if (tool_calls_str.empty()) {
            stmt.bind(5);  // NULL
        } else {
            stmt.bind(5, tool_calls_str);
        }
        if (msg.reasoning.has_value()) {
            stmt.bind(6, *msg.reasoning);
        } else {
            stmt.bind(6);
        }
        stmt.bind(7, static_cast<int64_t>(created_ms));
        stmt.exec();
        return static_cast<std::int64_t>(impl_->db->getLastInsertRowid());
    });
}

std::vector<MessageRow> SessionDB::get_messages(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SQLite::Statement q(*impl_->db, R"SQL(
        SELECT id, session_id, turn_index, role, content,
               tool_calls, reasoning, created_at
          FROM messages
         WHERE session_id = ?
         ORDER BY turn_index ASC, id ASC
    )SQL");
    q.bind(1, session_id);

    std::vector<MessageRow> rows;
    while (q.executeStep()) {
        MessageRow row;
        row.id = q.getColumn(0).getInt64();
        row.session_id = q.getColumn(1).getString();
        row.turn_index = q.getColumn(2).getInt();
        row.role = q.getColumn(3).getString();
        row.content = q.getColumn(4).isNull() ? std::string{}
                                                : q.getColumn(4).getString();
        if (!q.getColumn(5).isNull()) {
            std::string tc = q.getColumn(5).getString();
            row.tool_calls = tc.empty()
                                 ? nlohmann::json::array()
                                 : nlohmann::json::parse(tc, nullptr, false);
            if (row.tool_calls.is_discarded()) {
                row.tool_calls = nlohmann::json::array();
            }
        } else {
            row.tool_calls = nlohmann::json::array();
        }
        if (!q.getColumn(6).isNull()) {
            row.reasoning = q.getColumn(6).getString();
        }
        row.created_at = tp_from_millis(q.getColumn(7).getInt64());
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---- FTS5 ---------------------------------------------------------------

std::vector<FtsHit> SessionDB::fts_search(const std::string& query, int limit) {
    std::lock_guard<std::mutex> lk(impl_->mtx);

    std::vector<FtsHit> hits;
    SQLite::Statement q(*impl_->db, R"SQL(
        SELECT rowid,
               session_id,
               snippet(messages_fts, 0, '[', ']', '...', 10) AS snip,
               rank
          FROM messages_fts
         WHERE messages_fts MATCH ?
         ORDER BY rank
         LIMIT ?
    )SQL");
    q.bind(1, query);
    q.bind(2, limit);

    while (q.executeStep()) {
        FtsHit hit;
        hit.message_id = q.getColumn(0).getInt64();
        hit.session_id = q.getColumn(1).getString();
        hit.snippet = q.getColumn(2).getString();
        // FTS5 rank is negative; convert to a positive score (higher is
        // better) so callers can sort intuitively.
        hit.score = -q.getColumn(3).getDouble();
        hits.push_back(std::move(hit));
    }
    return hits;
}

// ---- Maintenance --------------------------------------------------------

void SessionDB::checkpoint() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->writes_since_checkpoint = 0;
    try {
        impl_->db->exec("PRAGMA wal_checkpoint(TRUNCATE);");
    } catch (const SQLite::Exception&) {
        // Swallow — a failed checkpoint is not actionable from here.
    }
}

int SessionDB::schema_version() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->read_schema_version();
}

}  // namespace hermes::state
