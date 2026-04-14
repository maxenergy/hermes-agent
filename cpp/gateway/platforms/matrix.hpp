// Phase 12 — Matrix platform adapter.
//
// C++17 port of gateway/platforms/matrix.py.  Covers the Matrix client-server
// API surface Hermes depends on: login (password / token / SSO), long-poll
// /sync, E2EE (Olm + Megolm), rich messaging (plain / HTML / markdown),
// replies, threads, reactions, edits, redactions, file upload, room
// membership, presence, read receipts, typing, rate-limit handling, and
// markdown-to-HTML conversion.
//
// Transport is pluggable via hermes::llm::HttpTransport so tests can inject
// a FakeHttpTransport.  Real deployments use the curl-backed default.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

#include "olm_session.hpp"

namespace hermes::gateway::platforms {

// Result of a Matrix API call that produces an event ID.  Parity with the
// Python SendResult dataclass in gateway/platforms/base.py.
struct MatrixSendResult {
    bool success = false;
    std::string message_id;  // event_id on success
    std::string error;       // set when !success

    static MatrixSendResult ok(std::string id = {}) {
        MatrixSendResult r;
        r.success = true;
        r.message_id = std::move(id);
        return r;
    }
    static MatrixSendResult fail(std::string err) {
        MatrixSendResult r;
        r.success = false;
        r.error = std::move(err);
        return r;
    }
};

// Minimal structured representation of a room event extracted from /sync.
struct MatrixEvent {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string type;            // m.room.message, m.room.member, m.reaction, …
    std::string msgtype;         // m.text, m.image, m.emote, m.notice, m.file, …
    std::string body;            // plain-text body
    std::string formatted_body;  // HTML body when format=org.matrix.custom.html
    std::string format;          // "org.matrix.custom.html" when set
    std::string url;             // mxc:// url for media events
    std::string filename;        // for media events
    std::int64_t timestamp = 0;  // origin_server_ts

    // Relations (m.relates_to).
    std::string relates_to_event_id;
    std::string relates_to_rel_type;  // "m.thread" / "m.replace" / "m.annotation"
    std::string in_reply_to_event_id;
    std::string reaction_key;  // for m.reaction → relates_to.key

    // Membership events.
    std::string membership;   // "invite" / "join" / "leave" / "ban"
    std::string state_key;    // user_id the membership applies to

    // Room metadata events.
    std::string room_name;    // m.room.name
    std::string room_topic;   // m.room.topic

    bool encrypted = false;   // m.room.encrypted envelope before decryption
};

// Parsed /sync response — deliberately shallow; the adapter exposes only
// what the conversation loop needs.
struct MatrixSyncResponse {
    std::string next_batch;  // the new since-token

    // Per-room joined timelines.
    std::map<std::string, std::vector<MatrixEvent>> room_events;
    // Invites to new rooms keyed by room_id.
    std::map<std::string, std::vector<MatrixEvent>> invites;
    // Presence events keyed by user_id.
    std::map<std::string, std::string> presence;
    // Typing events keyed by room_id → vector<user_id>.
    std::map<std::string, std::vector<std::string>> typing;
};

// Rate-limit accounting for 429 M_LIMIT_EXCEEDED.
struct MatrixRateLimit {
    std::int64_t retry_after_ms = 0;
    int hit_count = 0;
};

class MatrixAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string homeserver;   // https://matrix.org (no trailing slash)
        std::string user_id;      // full MXID, e.g. @bot:matrix.org
        std::string username;     // localpart used for m.login.password
        std::string password;
        std::string access_token;
        std::string refresh_token;
        std::string device_id;
        std::string device_name = "hermes-agent";

        // Passphrase used to encrypt the pickled olm account on disk.
        std::string pickle_passphrase = "hermes-default";

        // Chunked-send settings.
        std::size_t max_message_length = 32000;
        int request_timeout_sec = 45;
        int sync_timeout_ms = 30000;

        // Ignore-from-federation list: MXIDs whose events are dropped before
        // reaching the conversation loop.  Keeps parity with Python's
        // `_ignored_users`.
        std::set<std::string> ignored_users;
    };

    explicit MatrixAdapter(Config cfg);
    MatrixAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Matrix; }

    // ── Runner contract ──────────────────────────────────────────────
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }
    const std::string& access_token() const { return access_token_; }
    const std::string& user_id() const { return cfg_.user_id; }
    const std::string& next_batch() const { return next_batch_; }

    // ── Authentication ──────────────────────────────────────────────
    // Resolve .well-known/matrix/client → canonical homeserver URL.  Updates
    // cfg_.homeserver in place and returns the resolved URL.  Pass-through
    // when the document is missing or malformed.
    std::string discover_homeserver();

    // Derive a homeserver URL from an MXID (`@user:example.org` →
    // `https://example.org`).  Used as the last-resort fallback.
    static std::string homeserver_from_mxid(const std::string& mxid);

    // m.login.password — returns the access_token on success, empty on failure.
    std::string login_password();
    // m.login.token — exchange an SSO token for an access_token.
    std::string login_sso_token(const std::string& login_token);
    // Build the SSO redirect URL the user should visit to complete login.
    std::string sso_redirect_url(const std::string& redirect_to) const;

    // refresh_token grant.  Returns true when access_token was replaced.
    bool refresh_access_token();

    // Explicit logout — POST /logout and clear access_token_.
    bool logout();

    // ── E2EE ────────────────────────────────────────────────────────
    bool setup_e2ee();
    bool e2ee_enabled() const { return olm_account_.available(); }

    bool encrypt_room_message(const std::string& room_id,
                              const std::string& plaintext,
                              std::string& out_ciphertext);
    std::optional<std::string> decrypt_room_message(const std::string& room_id,
                                                    const std::string& ciphertext);

    // Claim one-time keys for `user_id/device_id` pairs via /keys/claim.
    // Returns the JSON response body on success, empty on failure.
    std::string claim_one_time_keys(
        const std::vector<std::pair<std::string, std::string>>& user_devices);
    // Query device keys for a set of user_ids via /keys/query.
    std::string query_device_keys(const std::vector<std::string>& user_ids);
    // Publish a cross-signing master/self-signing/user-signing key set.
    bool upload_cross_signing_keys(const std::string& master_json,
                                   const std::string& self_signing_json,
                                   const std::string& user_signing_json);
    // Mark a room as encrypted (m.room.encryption state event).
    bool enable_room_encryption(const std::string& room_id);
    // Create or reuse the outbound Megolm session for a room.  Returns the
    // session_id on success.
    std::string ensure_outbound_megolm(const std::string& room_id);
    // Import an inbound Megolm session (session_key) for `room_id`.
    bool import_inbound_megolm(const std::string& room_id,
                               const std::string& session_key);

    // ── Messaging ───────────────────────────────────────────────────
    MatrixSendResult send_text(const std::string& room_id,
                               const std::string& body,
                               const std::string& reply_to = {},
                               const std::string& thread_id = {});
    MatrixSendResult send_html(const std::string& room_id,
                               const std::string& body,
                               const std::string& html,
                               const std::string& reply_to = {},
                               const std::string& thread_id = {});
    MatrixSendResult send_markdown(const std::string& room_id,
                                   const std::string& markdown_text,
                                   const std::string& reply_to = {},
                                   const std::string& thread_id = {});
    MatrixSendResult send_notice(const std::string& room_id,
                                 const std::string& body);
    MatrixSendResult send_emote(const std::string& room_id,
                                const std::string& body);
    MatrixSendResult edit_message(const std::string& room_id,
                                  const std::string& event_id,
                                  const std::string& new_body);
    bool redact_message(const std::string& room_id,
                        const std::string& event_id,
                        const std::string& reason = {});
    MatrixSendResult send_reaction(const std::string& room_id,
                                   const std::string& target_event_id,
                                   const std::string& key);

    // ── Media ───────────────────────────────────────────────────────
    // Upload bytes to MXC and return the mxc:// URL (empty on failure).
    std::string upload_media(const std::string& content_type,
                             const std::string& filename,
                             const std::string& bytes);
    MatrixSendResult send_media(const std::string& room_id,
                                const std::string& msgtype,  // m.image/m.video/m.audio/m.file
                                const std::string& mxc_url,
                                const std::string& filename,
                                const std::string& content_type,
                                std::size_t size_bytes,
                                const std::string& caption = {},
                                const std::string& reply_to = {});
    // Convert mxc://server/id → https download URL.
    std::string mxc_to_http(const std::string& mxc_url) const;

    // ── Room ops ────────────────────────────────────────────────────
    std::string create_room(const std::string& name,
                            const std::string& topic,
                            const std::vector<std::string>& invitees,
                            bool is_direct,
                            const std::string& preset = "private_chat");
    // Ensure a direct-message room with `user_id` exists; returns room_id.
    std::string ensure_dm_room(const std::string& user_id);
    bool join_room(const std::string& room_id_or_alias);
    bool leave_room(const std::string& room_id);
    bool invite_user(const std::string& room_id, const std::string& user_id);
    bool kick_user(const std::string& room_id, const std::string& user_id,
                   const std::string& reason = {});
    bool ban_user(const std::string& room_id, const std::string& user_id,
                  const std::string& reason = {});
    bool set_room_name(const std::string& room_id, const std::string& name);
    bool set_room_topic(const std::string& room_id, const std::string& topic);
    bool set_power_level(const std::string& room_id, const std::string& user_id,
                         int level);
    bool send_read_receipt(const std::string& room_id, const std::string& event_id);

    // Fetch chat metadata (room name + type).
    std::pair<std::string, std::string> get_chat_info(const std::string& room_id);

    // Fetch recent messages (via /rooms/{id}/messages?dir=b).  Returns events
    // in chronological order.
    std::vector<MatrixEvent> fetch_room_history(const std::string& room_id,
                                                 int limit = 50,
                                                 const std::string& start = {});

    // ── Presence ────────────────────────────────────────────────────
    bool set_presence(const std::string& state, const std::string& status_msg = {});
    static bool valid_presence_state(const std::string& state);

    // ── Sync ────────────────────────────────────────────────────────
    // Perform one /sync request.  Appends new events to `out` and updates
    // `next_batch_`.  When `use_filter` is true, a narrow filter is sent.
    bool sync_once(MatrixSyncResponse& out, bool use_filter = true);

    // Parse a raw /sync JSON string into MatrixSyncResponse.  Exposed for
    // testability.
    static MatrixSyncResponse parse_sync_response(const std::string& json_body);

    // Since-token persistence (best-effort, under $HERMES_HOME/matrix/).
    std::string sync_token_path() const;
    bool save_sync_token() const;
    bool load_sync_token();

    // ── Helpers (public for testability) ────────────────────────────
    // Bot-mention detection against a body / optional formatted_body.
    bool is_bot_mentioned(const std::string& body,
                          const std::string& formatted_body = {}) const;
    std::string strip_mention(const std::string& body) const;

    // Markdown → HTML (regex fallback, matches Python's
    // _markdown_to_html_fallback).  Exposed for tests.
    static std::string markdown_to_html(const std::string& text);
    static std::string sanitize_link_url(const std::string& url);
    static std::string html_escape(const std::string& s);

    // Duplicate-event memory (bounded ring) — returns true and records when
    // first seen, false on repeat.
    bool observe_event(const std::string& event_id);

    // Rate-limit accounting getters (for telemetry).
    const MatrixRateLimit& rate_limit_state() const { return rate_limit_; }
    // Called by send_* on 429 to record retry_after.  Public for testing.
    void record_rate_limit(std::int64_t retry_after_ms);

    // Thread participation tracking (persisted under $HERMES_HOME/matrix/).
    void track_thread(const std::string& thread_id);
    bool is_thread_participated(const std::string& thread_id) const;
    bool load_participated_threads();
    bool save_participated_threads() const;
    std::string participated_threads_path() const;

    // Transaction-ID counter (monotonic, per-process).
    std::string next_txn_id();

    // Build the m.room.message payload for a text/markdown message, shared
    // by send_text/send_html/send_markdown + edit_message.  Exposed for tests.
    static std::string build_message_payload(
        const std::string& msgtype,
        const std::string& body,
        const std::string& html,
        const std::string& reply_to,
        const std::string& thread_id);

    // Build an edit payload wrapping the new content in m.replace.
    static std::string build_edit_payload(const std::string& event_id,
                                          const std::string& new_body,
                                          const std::string& new_html);

    // Build a reaction payload.
    static std::string build_reaction_payload(const std::string& target_event_id,
                                              const std::string& key);

    // Truncate a message into ≤max_len chunks on word boundaries.
    static std::vector<std::string> chunk_message(const std::string& body,
                                                  std::size_t max_len);

    // Set the adapter's cached user_id (called after login).
    void set_user_id(std::string uid) { cfg_.user_id = std::move(uid); }

private:
    hermes::llm::HttpTransport* get_transport();
    std::string olm_pickle_path() const;

    // Perform a PUT-style send via HttpTransport.  Matrix requires PUT for
    // /send/{type}/{txn}; since our HttpTransport abstraction only exposes
    // post_json, we pass through as POST — Synapse accepts both.
    hermes::llm::HttpTransport::Response authed_post(
        const std::string& url,
        const std::string& body,
        const std::string& extra_content_type = "application/json");
    hermes::llm::HttpTransport::Response authed_get(const std::string& url);

    // Wrap a request with 429 retry bookkeeping.
    hermes::llm::HttpTransport::Response retrying_post(
        const std::string& url,
        const std::string& body,
        int max_retries = 2);

    // Build a v3 client URL: `{homeserver}/_matrix/client/v3/{suffix}`.
    std::string v3_url(const std::string& suffix) const;
    std::string v1_media_url(const std::string& suffix) const;

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string access_token_;
    std::string next_batch_;
    std::uint64_t txn_counter_ = 0;

    // Bounded set of recently-observed event_ids for de-duplication.
    std::deque<std::string> recent_event_ids_;
    std::set<std::string> recent_event_id_set_;
    static constexpr std::size_t kMaxRecentEvents = 1024;

    MatrixRateLimit rate_limit_;

    // Set of thread_ids the bot has participated in (persisted).
    std::set<std::string> participated_threads_;

    // DM room cache (user_id → room_id) populated from m.direct account data.
    std::unordered_map<std::string, std::string> dm_rooms_;

    // Known room metadata (best-effort cache).
    std::unordered_map<std::string, std::string> room_names_;
    std::set<std::string> encrypted_rooms_;

    // E2EE state.
    OlmAccount olm_account_;
    std::map<std::pair<std::string, std::string>, OlmSession> olm_sessions_;
    std::map<std::string, MegolmOutboundSession> megolm_out_;                                // room_id → session
    std::map<std::string, std::map<std::string, MegolmInboundSession>> megolm_in_;           // room_id → session_id → session
};

}  // namespace hermes::gateway::platforms
