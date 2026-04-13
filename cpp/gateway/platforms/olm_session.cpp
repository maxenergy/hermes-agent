// Phase 12 — Matrix E2EE wrappers (see olm_session.hpp).
#include "olm_session.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#ifdef HERMES_GATEWAY_HAS_OLM
extern "C" {
#include <olm/olm.h>
#include <olm/outbound_group_session.h>
#include <olm/inbound_group_session.h>
}
#endif

namespace hermes::gateway::platforms {

#ifdef HERMES_GATEWAY_HAS_OLM
namespace {

// Fill buffer with cryptographically-random bytes. libolm itself does not
// provide entropy; the caller must.  For tests a non-crypto std::mt19937
// seeded from std::random_device is adequate; production callers that need
// stronger guarantees should link against a CSPRNG.
std::vector<std::uint8_t> random_bytes(std::size_t n) {
    std::vector<std::uint8_t> buf(n);
    if (n == 0) return buf;
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<std::uint8_t>(rd());
    }
    return buf;
}

inline ::OlmAccount* as_account(void* p) {
    return static_cast<::OlmAccount*>(p);
}
inline ::OlmSession* as_session(void* p) {
    return static_cast<::OlmSession*>(p);
}
inline OlmOutboundGroupSession* as_outbound(void* p) {
    return static_cast<OlmOutboundGroupSession*>(p);
}
inline OlmInboundGroupSession* as_inbound(void* p) {
    return static_cast<OlmInboundGroupSession*>(p);
}

}  // namespace
#endif

// =======================================================================
// OlmAccount
// =======================================================================

OlmAccount::OlmAccount() {
#ifdef HERMES_GATEWAY_HAS_OLM
    void* mem = std::malloc(olm_account_size());
    if (!mem) return;
    account_ = olm_account(mem);
    auto seed = random_bytes(olm_create_account_random_length(as_account(account_)));
    if (olm_create_account(as_account(account_), seed.data(), seed.size()) ==
        olm_error()) {
        olm_clear_account(as_account(account_));
        std::free(mem);
        account_ = nullptr;
    }
#endif
}

OlmAccount::~OlmAccount() {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (account_) {
        void* mem = account_;
        olm_clear_account(as_account(account_));
        std::free(mem);
    }
#endif
}

OlmAccount::OlmAccount(OlmAccount&& other) noexcept : account_(other.account_) {
    other.account_ = nullptr;
}

OlmAccount& OlmAccount::operator=(OlmAccount&& other) noexcept {
    if (this != &other) {
#ifdef HERMES_GATEWAY_HAS_OLM
        if (account_) {
            void* mem = account_;
            olm_clear_account(as_account(account_));
            std::free(mem);
        }
#endif
        account_ = other.account_;
        other.account_ = nullptr;
    }
    return *this;
}

bool OlmAccount::available() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    return account_ != nullptr;
#else
    return false;
#endif
}

void* OlmAccount::raw() const {
    return account_;
}

std::string OlmAccount::pickle(const std::string& passphrase) const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) return {};
    std::size_t len = olm_pickle_account_length(as_account(account_));
    std::string out(len, '\0');
    auto r = olm_pickle_account(as_account(account_),
                                 passphrase.data(), passphrase.size(),
                                 out.data(), out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    (void)passphrase;
    return {};
#endif
}

bool OlmAccount::unpickle(const std::string& pickled, const std::string& passphrase) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) {
        void* mem = std::malloc(olm_account_size());
        if (!mem) return false;
        account_ = olm_account(mem);
    }
    // olm_unpickle_account mutates the input buffer; copy it.
    std::string buf = pickled;
    auto r = olm_unpickle_account(as_account(account_),
                                   passphrase.data(), passphrase.size(),
                                   buf.data(), buf.size());
    return r != olm_error();
#else
    (void)pickled;
    (void)passphrase;
    return false;
#endif
}

std::string OlmAccount::identity_keys_json() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) return {};
    std::size_t len = olm_account_identity_keys_length(as_account(account_));
    std::string out(len, '\0');
    auto r = olm_account_identity_keys(as_account(account_), out.data(), out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    return {};
#endif
}

bool OlmAccount::generate_one_time_keys(std::size_t count) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) return false;
    std::size_t rand_len =
        olm_account_generate_one_time_keys_random_length(as_account(account_), count);
    auto rnd = random_bytes(rand_len);
    auto r = olm_account_generate_one_time_keys(as_account(account_), count,
                                                 rnd.data(), rnd.size());
    return r != olm_error();
#else
    (void)count;
    return false;
#endif
}

std::string OlmAccount::one_time_keys_json() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) return {};
    std::size_t len = olm_account_one_time_keys_length(as_account(account_));
    std::string out(len, '\0');
    auto r = olm_account_one_time_keys(as_account(account_), out.data(), out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    return {};
#endif
}

void OlmAccount::mark_keys_as_published() {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account_) return;
    olm_account_mark_keys_as_published(as_account(account_));
#endif
}

// =======================================================================
// OlmSession
// =======================================================================

OlmSession::OlmSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    void* mem = std::malloc(olm_session_size());
    if (!mem) return;
    session_ = olm_session(mem);
#endif
}

OlmSession::~OlmSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (session_) {
        void* mem = session_;
        olm_clear_session(as_session(session_));
        std::free(mem);
    }
#endif
}

OlmSession::OlmSession(OlmSession&& other) noexcept : session_(other.session_) {
    other.session_ = nullptr;
}

OlmSession& OlmSession::operator=(OlmSession&& other) noexcept {
    if (this != &other) {
#ifdef HERMES_GATEWAY_HAS_OLM
        if (session_) {
            void* mem = session_;
            olm_clear_session(as_session(session_));
            std::free(mem);
        }
#endif
        session_ = other.session_;
        other.session_ = nullptr;
    }
    return *this;
}

bool OlmSession::available() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    return session_ != nullptr;
#else
    return false;
#endif
}

std::optional<OlmSession> OlmSession::create_outbound(const OlmAccount& account,
                                                     const std::string& their_identity_key,
                                                     const std::string& their_one_time_key) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account.available()) return std::nullopt;
    OlmSession s;
    if (!s.session_) return std::nullopt;
    std::size_t rand_len =
        olm_create_outbound_session_random_length(as_session(s.session_));
    auto rnd = random_bytes(rand_len);
    auto r = olm_create_outbound_session(
        as_session(s.session_), as_account(account.raw()),
        their_identity_key.data(), their_identity_key.size(),
        their_one_time_key.data(), their_one_time_key.size(),
        rnd.data(), rnd.size());
    if (r == olm_error()) return std::nullopt;
    return s;
#else
    (void)account;
    (void)their_identity_key;
    (void)their_one_time_key;
    return std::nullopt;
#endif
}

std::optional<OlmSession> OlmSession::create_inbound(OlmAccount& account,
                                                     const std::string& their_identity_key,
                                                     const std::string& one_time_key_message) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!account.available()) return std::nullopt;
    OlmSession s;
    if (!s.session_) return std::nullopt;
    // olm_create_inbound_session_from mutates its input; copy.
    std::string msg_copy = one_time_key_message;
    auto r = olm_create_inbound_session_from(
        as_session(s.session_), as_account(account.raw()),
        their_identity_key.data(), their_identity_key.size(),
        msg_copy.data(), msg_copy.size());
    if (r == olm_error()) return std::nullopt;
    // Remove the OTK so it can't be reused.
    olm_remove_one_time_keys(as_account(account.raw()), as_session(s.session_));
    return s;
#else
    (void)account;
    (void)their_identity_key;
    (void)one_time_key_message;
    return std::nullopt;
#endif
}

std::string OlmSession::encrypt(const std::string& plaintext) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t msg_type = olm_encrypt_message_type(as_session(session_));
    std::size_t rand_len = olm_encrypt_random_length(as_session(session_));
    std::size_t cipher_len =
        olm_encrypt_message_length(as_session(session_), plaintext.size());
    auto rnd = random_bytes(rand_len);
    std::string body(cipher_len, '\0');
    auto r = olm_encrypt(as_session(session_),
                          plaintext.data(), plaintext.size(),
                          rnd.data(), rnd.size(),
                          body.data(), body.size());
    if (r == olm_error()) return {};
    body.resize(r);
    nlohmann::json out = {
        {"type", static_cast<int>(msg_type)},
        {"body", body}
    };
    return out.dump();
#else
    (void)plaintext;
    return {};
#endif
}

std::optional<std::string> OlmSession::decrypt(int type, const std::string& body) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return std::nullopt;
    // olm_decrypt_max_plaintext_length mutates the body, so copy twice.
    std::string body_copy1 = body;
    auto max_plain = olm_decrypt_max_plaintext_length(
        as_session(session_), static_cast<std::size_t>(type),
        body_copy1.data(), body_copy1.size());
    if (max_plain == olm_error()) return std::nullopt;

    std::string body_copy2 = body;
    std::string out(max_plain, '\0');
    auto r = olm_decrypt(as_session(session_),
                          static_cast<std::size_t>(type),
                          body_copy2.data(), body_copy2.size(),
                          out.data(), out.size());
    if (r == olm_error()) return std::nullopt;
    out.resize(r);
    return out;
#else
    (void)type;
    (void)body;
    return std::nullopt;
#endif
}

std::string OlmSession::pickle(const std::string& passphrase) const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t len = olm_pickle_session_length(as_session(session_));
    std::string out(len, '\0');
    auto r = olm_pickle_session(as_session(session_),
                                 passphrase.data(), passphrase.size(),
                                 out.data(), out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    (void)passphrase;
    return {};
#endif
}

bool OlmSession::unpickle(const std::string& pickled, const std::string& passphrase) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) {
        void* mem = std::malloc(olm_session_size());
        if (!mem) return false;
        session_ = olm_session(mem);
    }
    std::string buf = pickled;
    auto r = olm_unpickle_session(as_session(session_),
                                   passphrase.data(), passphrase.size(),
                                   buf.data(), buf.size());
    return r != olm_error();
#else
    (void)pickled;
    (void)passphrase;
    return false;
#endif
}

bool OlmSession::matches_inbound_session(const std::string& msg) const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return false;
    std::string copy = msg;
    auto r = olm_matches_inbound_session(as_session(session_),
                                          copy.data(), copy.size());
    return r == 1;
#else
    (void)msg;
    return false;
#endif
}

// =======================================================================
// MegolmOutboundSession
// =======================================================================

MegolmOutboundSession::MegolmOutboundSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    void* mem = std::malloc(olm_outbound_group_session_size());
    if (!mem) return;
    session_ = olm_outbound_group_session(mem);
    std::size_t rand_len =
        olm_init_outbound_group_session_random_length(as_outbound(session_));
    auto rnd = random_bytes(rand_len);
    auto r = olm_init_outbound_group_session(as_outbound(session_),
                                              rnd.data(), rnd.size());
    if (r == olm_error()) {
        olm_clear_outbound_group_session(as_outbound(session_));
        std::free(mem);
        session_ = nullptr;
    }
#endif
}

MegolmOutboundSession::~MegolmOutboundSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (session_) {
        void* mem = session_;
        olm_clear_outbound_group_session(as_outbound(session_));
        std::free(mem);
    }
#endif
}

MegolmOutboundSession::MegolmOutboundSession(MegolmOutboundSession&& other) noexcept
    : session_(other.session_) {
    other.session_ = nullptr;
}

MegolmOutboundSession& MegolmOutboundSession::operator=(MegolmOutboundSession&& other) noexcept {
    if (this != &other) {
#ifdef HERMES_GATEWAY_HAS_OLM
        if (session_) {
            void* mem = session_;
            olm_clear_outbound_group_session(as_outbound(session_));
            std::free(mem);
        }
#endif
        session_ = other.session_;
        other.session_ = nullptr;
    }
    return *this;
}

bool MegolmOutboundSession::available() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    return session_ != nullptr;
#else
    return false;
#endif
}

std::string MegolmOutboundSession::session_id() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t len = olm_outbound_group_session_id_length(as_outbound(session_));
    std::string out(len, '\0');
    auto r = olm_outbound_group_session_id(as_outbound(session_),
                                            reinterpret_cast<std::uint8_t*>(out.data()),
                                            out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    return {};
#endif
}

std::string MegolmOutboundSession::session_key() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t len = olm_outbound_group_session_key_length(as_outbound(session_));
    std::string out(len, '\0');
    auto r = olm_outbound_group_session_key(as_outbound(session_),
                                             reinterpret_cast<std::uint8_t*>(out.data()),
                                             out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    return {};
#endif
}

std::uint32_t MegolmOutboundSession::message_index() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return 0;
    return olm_outbound_group_session_message_index(as_outbound(session_));
#else
    return 0;
#endif
}

std::string MegolmOutboundSession::encrypt(const std::string& plaintext) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t cipher_len =
        olm_group_encrypt_message_length(as_outbound(session_), plaintext.size());
    std::string out(cipher_len, '\0');
    auto r = olm_group_encrypt(as_outbound(session_),
                                reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                                plaintext.size(),
                                reinterpret_cast<std::uint8_t*>(out.data()),
                                out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    (void)plaintext;
    return {};
#endif
}

std::string MegolmOutboundSession::pickle(const std::string& passphrase) const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t len = olm_pickle_outbound_group_session_length(as_outbound(session_));
    std::string out(len, '\0');
    auto r = olm_pickle_outbound_group_session(as_outbound(session_),
                                                passphrase.data(), passphrase.size(),
                                                out.data(), out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    (void)passphrase;
    return {};
#endif
}

bool MegolmOutboundSession::unpickle(const std::string& pickled, const std::string& passphrase) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) {
        void* mem = std::malloc(olm_outbound_group_session_size());
        if (!mem) return false;
        session_ = olm_outbound_group_session(mem);
    }
    std::string buf = pickled;
    auto r = olm_unpickle_outbound_group_session(as_outbound(session_),
                                                  passphrase.data(), passphrase.size(),
                                                  buf.data(), buf.size());
    return r != olm_error();
#else
    (void)pickled;
    (void)passphrase;
    return false;
#endif
}

// =======================================================================
// MegolmInboundSession
// =======================================================================

MegolmInboundSession::MegolmInboundSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    void* mem = std::malloc(olm_inbound_group_session_size());
    if (!mem) return;
    session_ = olm_inbound_group_session(mem);
#endif
}

MegolmInboundSession::~MegolmInboundSession() {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (session_) {
        void* mem = session_;
        olm_clear_inbound_group_session(as_inbound(session_));
        std::free(mem);
    }
#endif
}

MegolmInboundSession::MegolmInboundSession(MegolmInboundSession&& other) noexcept
    : session_(other.session_) {
    other.session_ = nullptr;
}

MegolmInboundSession& MegolmInboundSession::operator=(MegolmInboundSession&& other) noexcept {
    if (this != &other) {
#ifdef HERMES_GATEWAY_HAS_OLM
        if (session_) {
            void* mem = session_;
            olm_clear_inbound_group_session(as_inbound(session_));
            std::free(mem);
        }
#endif
        session_ = other.session_;
        other.session_ = nullptr;
    }
    return *this;
}

bool MegolmInboundSession::available() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    return session_ != nullptr;
#else
    return false;
#endif
}

bool MegolmInboundSession::init_from_session_key(const std::string& session_key) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return false;
    std::string copy = session_key;
    auto r = olm_init_inbound_group_session(as_inbound(session_),
                                             reinterpret_cast<const std::uint8_t*>(copy.data()),
                                             copy.size());
    return r != olm_error();
#else
    (void)session_key;
    return false;
#endif
}

std::optional<std::string> MegolmInboundSession::decrypt(const std::string& ciphertext) {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return std::nullopt;
    std::string copy1 = ciphertext;
    auto max_plain = olm_group_decrypt_max_plaintext_length(
        as_inbound(session_),
        reinterpret_cast<std::uint8_t*>(copy1.data()), copy1.size());
    if (max_plain == olm_error()) return std::nullopt;

    std::string copy2 = ciphertext;
    std::string out(max_plain, '\0');
    std::uint32_t msg_index = 0;
    auto r = olm_group_decrypt(as_inbound(session_),
                                reinterpret_cast<std::uint8_t*>(copy2.data()), copy2.size(),
                                reinterpret_cast<std::uint8_t*>(out.data()), out.size(),
                                &msg_index);
    if (r == olm_error()) return std::nullopt;
    out.resize(r);
    return out;
#else
    (void)ciphertext;
    return std::nullopt;
#endif
}

std::string MegolmInboundSession::session_id() const {
#ifdef HERMES_GATEWAY_HAS_OLM
    if (!session_) return {};
    std::size_t len = olm_inbound_group_session_id_length(as_inbound(session_));
    std::string out(len, '\0');
    auto r = olm_inbound_group_session_id(as_inbound(session_),
                                           reinterpret_cast<std::uint8_t*>(out.data()),
                                           out.size());
    if (r == olm_error()) return {};
    out.resize(r);
    return out;
#else
    return {};
#endif
}

}  // namespace hermes::gateway::platforms
