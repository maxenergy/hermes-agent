// Phase 12 — Weixin platform adapter implementation (depth port).
#include "weixin.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef __has_include
#  if __has_include(<openssl/aes.h>)
#    include <openssl/aes.h>
#    include <openssl/evp.h>
#    define HERMES_WEIXIN_HAS_OPENSSL 1
#  endif
#endif

namespace hermes::gateway::platforms {

namespace {

std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string base64_encode(const std::string& bytes) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : bytes) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64_decode(const std::string& input) {
    static int T[256];
    static bool init = false;
    if (!init) {
        std::fill(std::begin(T), std::end(T), -1);
        const char* tbl =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(tbl[i])] = i;
        init = true;
    }
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (T[c] == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) | T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free-function helpers
// ---------------------------------------------------------------------------

std::string weixin_safe_id(const std::string& value, std::size_t keep) {
    std::string trimmed = value;
    // trim whitespace
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) return "?";
    if (trimmed.size() <= keep) return trimmed;
    return trimmed.substr(0, keep);
}

std::string weixin_json_dumps(const nlohmann::json& payload) {
    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string weixin_pkcs7_pad(const std::string& data, std::size_t block) {
    std::size_t pad_len = block - (data.size() % block);
    return data + std::string(pad_len, static_cast<char>(pad_len));
}

std::string weixin_pkcs7_unpad(const std::string& data, std::size_t block) {
    if (data.empty()) return data;
    unsigned char last = static_cast<unsigned char>(data.back());
    if (last == 0 || last > block) return data;
    if (data.size() < last) return data;
    for (std::size_t i = data.size() - last; i < data.size(); ++i) {
        if (static_cast<unsigned char>(data[i]) != last) return data;
    }
    return data.substr(0, data.size() - last);
}

std::string weixin_aes128_ecb_encrypt(const std::string& plaintext,
                                      const std::string& key) {
#ifdef HERMES_WEIXIN_HAS_OPENSSL
    if (key.size() != 16) return {};
    std::string padded = weixin_pkcs7_pad(plaintext, 16);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    std::string out(padded.size() + 16, '\0');
    int out_len = 0, total = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                          &out_len,
                          reinterpret_cast<const unsigned char*>(padded.data()),
                          static_cast<int>(padded.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    total = out_len;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
                            &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    total += out_len;
    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<std::size_t>(total));
    return out;
#else
    (void)plaintext; (void)key;
    return {};
#endif
}

std::string weixin_aes128_ecb_decrypt(const std::string& ciphertext,
                                      const std::string& key) {
#ifdef HERMES_WEIXIN_HAS_OPENSSL
    if (key.size() != 16) return {};
    if (ciphertext.size() % 16 != 0) return {};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    std::string out(ciphertext.size() + 16, '\0');
    int out_len = 0, total = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                          &out_len,
                          reinterpret_cast<const unsigned char*>(ciphertext.data()),
                          static_cast<int>(ciphertext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    total = out_len;
    if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
                            &out_len) != 1) {
        // ignore final errors — we handle unpadding ourselves
    }
    total += out_len;
    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<std::size_t>(total));
    return weixin_pkcs7_unpad(out, 16);
#else
    (void)ciphertext; (void)key;
    return {};
#endif
}

std::size_t weixin_aes_padded_size(std::size_t n) {
    return ((n + 1 + 15) / 16) * 16;
}

std::string weixin_random_uin() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t v = dist(rng);
    std::string as_digits = std::to_string(v);
    return base64_encode(as_digits);
}

std::unordered_map<std::string, std::string> weixin_headers(
    const std::optional<std::string>& token, const std::string& body) {
    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["AuthorizationType"] = "ilink_bot_token";
    headers["Content-Length"] = std::to_string(body.size());
    headers["X-WECHAT-UIN"] = weixin_random_uin();
    headers["iLink-App-Id"] = kILinkAppId;
    headers["iLink-App-ClientVersion"] = std::to_string(kILinkAppClientVersion);
    if (token && !token->empty()) {
        headers["Authorization"] = "Bearer " + *token;
    }
    return headers;
}

std::string weixin_parse_aes_key(const std::string& aes_key_b64) {
    std::string decoded = base64_decode(aes_key_b64);
    if (decoded.size() == 16) return decoded;
    if (decoded.size() == 32) {
        bool all_hex = true;
        for (char c : decoded) {
            if (!is_hex_char(c)) { all_hex = false; break; }
        }
        if (all_hex) {
            std::string bytes;
            bytes.reserve(16);
            for (std::size_t i = 0; i + 1 < decoded.size(); i += 2) {
                char buf[3] = {decoded[i], decoded[i + 1], 0};
                bytes.push_back(static_cast<char>(std::strtol(buf, nullptr, 16)));
            }
            return bytes;
        }
    }
    return {};
}

std::string weixin_url_quote(std::string_view value) {
    std::ostringstream out;
    out.fill('0');
    out << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::uppercase
                << static_cast<int>(c) << std::nouppercase;
        }
    }
    return out.str();
}

std::string weixin_cdn_download_url(const std::string& cdn_base_url,
                                    const std::string& encrypted_query_param) {
    std::string base = cdn_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/download?encrypted_query_param=" +
           weixin_url_quote(encrypted_query_param);
}

std::string weixin_cdn_upload_url(const std::string& cdn_base_url,
                                  const std::string& upload_param,
                                  const std::string& filekey) {
    std::string base = cdn_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/upload?encrypted_query_param=" +
           weixin_url_quote(upload_param) +
           "&filekey=" + weixin_url_quote(filekey);
}

WeixinChatKind weixin_guess_chat_type(const nlohmann::json& message,
                                      const std::string& account_id) {
    auto str = [&](const char* k) -> std::string {
        if (!message.contains(k)) return {};
        auto& v = message.at(k);
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        return {};
    };
    std::string room_id = str("room_id");
    if (room_id.empty()) room_id = str("chat_room_id");
    std::string to_user_id = str("to_user_id");
    std::string from_user_id = str("from_user_id");
    int msg_type = message.value("msg_type", 0);
    bool is_group = !room_id.empty() ||
                    (!to_user_id.empty() && !account_id.empty() &&
                     to_user_id != account_id && msg_type == 1);
    WeixinChatKind out;
    if (is_group) {
        out.kind = "group";
        if (!room_id.empty()) out.chat_id = room_id;
        else if (!to_user_id.empty()) out.chat_id = to_user_id;
        else out.chat_id = from_user_id;
    } else {
        out.kind = "dm";
        out.chat_id = from_user_id;
    }
    return out;
}

std::vector<std::string> weixin_split_table_row(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    bool prev_escape = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (!prev_escape && c == '|') {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        prev_escape = (c == '\\');
        cur.push_back(c);
    }
    parts.push_back(cur);
    // Trim leading/trailing whitespace from each cell.
    for (auto& p : parts) {
        while (!p.empty() && std::isspace(static_cast<unsigned char>(p.front()))) p.erase(p.begin());
        while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.pop_back();
    }
    // Drop leading + trailing empty produced by "| a | b |".
    if (!parts.empty() && parts.front().empty()) parts.erase(parts.begin());
    if (!parts.empty() && parts.back().empty()) parts.pop_back();
    return parts;
}

std::string weixin_rewrite_headers(const std::string& line) {
    // ATX-style "# h", "## h", ... -> plain bold/emphasis markers.
    std::size_t i = 0;
    while (i < line.size() && line[i] == '#') ++i;
    if (i == 0 || i > 6) return line;
    if (i >= line.size() || line[i] != ' ') return line;
    std::string text = line.substr(i + 1);
    // Prefix with U+25B8 bullet-style lead and wrap in asterisks.
    return "*" + text + "*";
}

std::string weixin_rewrite_table_block(const std::vector<std::string>& lines) {
    if (lines.size() < 2) return {};
    std::vector<std::string> headers = weixin_split_table_row(lines[0]);
    // lines[1] is the separator — skip.
    std::ostringstream out;
    for (std::size_t r = 2; r < lines.size(); ++r) {
        auto cells = weixin_split_table_row(lines[r]);
        for (std::size_t c = 0; c < cells.size(); ++c) {
            std::string key = (c < headers.size()) ? headers[c]
                                                   : "col" + std::to_string(c + 1);
            out << key << ": " << cells[c];
            if (c + 1 < cells.size()) out << "\n";
        }
        if (r + 1 < lines.size()) out << "\n\n";
    }
    return out.str();
}

namespace {
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

bool looks_like_table_row(const std::string& line) {
    auto first = line.find_first_not_of(" \t");
    return first != std::string::npos && line[first] == '|';
}

bool looks_like_table_separator(const std::string& line) {
    // "| --- | --- |" style.
    if (!looks_like_table_row(line)) return false;
    for (char c : line) {
        if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
    }
    return true;
}
}  // namespace

std::string weixin_normalize_markdown_blocks(const std::string& content) {
    auto lines = split_lines(content);
    std::ostringstream out;
    std::vector<std::string> table_buf;
    auto flush_table = [&]() {
        if (!table_buf.empty()) {
            out << weixin_rewrite_table_block(table_buf);
            out << "\n";
            table_buf.clear();
        }
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (looks_like_table_row(line) && i + 1 < lines.size() &&
            looks_like_table_separator(lines[i + 1])) {
            // Start of a table.
            table_buf.clear();
            table_buf.push_back(line);
            table_buf.push_back(lines[i + 1]);
            i += 1;
            while (i + 1 < lines.size() && looks_like_table_row(lines[i + 1])) {
                ++i;
                table_buf.push_back(lines[i]);
            }
            flush_table();
            continue;
        }
        out << weixin_rewrite_headers(line) << "\n";
    }
    std::string s = out.str();
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

std::vector<std::string> weixin_split_markdown_blocks(const std::string& content) {
    // Split on blank lines — preserves fenced code blocks intact.
    std::vector<std::string> blocks;
    std::string cur;
    bool in_fence = false;
    auto lines = split_lines(content);
    for (const auto& line : lines) {
        bool is_fence_line = line.size() >= 3 && line.substr(0, 3) == "```";
        if (is_fence_line) in_fence = !in_fence;
        if (line.empty() && !in_fence) {
            if (!cur.empty()) {
                blocks.push_back(cur);
                cur.clear();
            }
        } else {
            if (!cur.empty()) cur.push_back('\n');
            cur += line;
        }
    }
    if (!cur.empty()) blocks.push_back(cur);
    return blocks;
}

std::vector<std::string> weixin_pack_markdown_blocks(const std::string& content,
                                                     std::size_t max_length) {
    auto blocks = weixin_split_markdown_blocks(content);
    std::vector<std::string> out;
    std::string cur;
    for (auto& b : blocks) {
        if (b.size() > max_length) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            // Hard-split huge single block.
            for (std::size_t i = 0; i < b.size(); i += max_length) {
                out.push_back(b.substr(i, max_length));
            }
            continue;
        }
        if (cur.size() + 2 + b.size() > max_length && !cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
        if (!cur.empty()) cur += "\n\n";
        cur += b;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> weixin_split_text_for_delivery(
    const std::string& content, std::size_t max_length) {
    if (content.size() <= max_length) return {content};
    std::string normalized = weixin_normalize_markdown_blocks(content);
    auto packed = weixin_pack_markdown_blocks(normalized, max_length);
    if (!packed.empty()) return packed;
    // Fallback: simple slicing.
    std::vector<std::string> out;
    for (std::size_t i = 0; i < content.size(); i += max_length) {
        out.push_back(content.substr(i, max_length));
    }
    return out;
}

std::string weixin_extract_text(const nlohmann::json& item_list) {
    std::string out;
    if (!item_list.is_array()) return out;
    for (const auto& it : item_list) {
        if (it.contains("text_item") && it["text_item"].contains("text") &&
            it["text_item"]["text"].is_string()) {
            if (!out.empty()) out.push_back('\n');
            out += it["text_item"]["text"].get<std::string>();
        }
    }
    return out;
}

std::string weixin_mime_from_filename(const std::string& filename) {
    auto pos = filename.rfind('.');
    if (pos == std::string::npos) return "application/octet-stream";
    std::string ext = to_lower_copy(filename.substr(pos + 1));
    static const std::unordered_map<std::string, std::string> kMap = {
        {"png", "image/png"},    {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},    {"webp", "image/webp"}, {"mp4", "video/mp4"},
        {"mov", "video/quicktime"}, {"mp3", "audio/mpeg"}, {"aac", "audio/aac"},
        {"amr", "audio/amr"},    {"wav", "audio/wav"},   {"pdf", "application/pdf"},
        {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"ppt", "application/vnd.ms-powerpoint"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"txt", "text/plain"},   {"zip", "application/zip"},
        {"json", "application/json"},
    };
    auto it = kMap.find(ext);
    if (it != kMap.end()) return it->second;
    return "application/octet-stream";
}

std::string weixin_extract_xml_tag(const std::string& xml,
                                   const std::string& tag) {
    std::string cdata_open = "<" + tag + "><![CDATA[";
    std::string cdata_close = "]]></" + tag + ">";
    auto pos = xml.find(cdata_open);
    if (pos != std::string::npos) {
        auto start = pos + cdata_open.size();
        auto end = xml.find(cdata_close, start);
        if (end != std::string::npos) return xml.substr(start, end - start);
    }
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    pos = xml.find(open);
    if (pos != std::string::npos) {
        auto start = pos + open.size();
        auto end = xml.find(close, start);
        if (end != std::string::npos) return xml.substr(start, end - start);
    }
    return {};
}

std::string weixin_message_type_from_media(
    const std::vector<std::string>& media_types, const std::string& text) {
    if (!media_types.empty()) {
        const auto& first = media_types[0];
        if (first == "image") return "PHOTO";
        if (first == "video") return "VIDEO";
        if (first == "voice") return "VOICE";
        if (first == "file") return "DOCUMENT";
        if (first == "sticker") return "STICKER";
    }
    (void)text;
    return "TEXT";
}

// ---------------------------------------------------------------------------
// Account persistence
// ---------------------------------------------------------------------------

std::filesystem::path weixin_account_dir(const std::string& hermes_home) {
    std::filesystem::path p =
        std::filesystem::path(hermes_home) / "weixin" / "accounts";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

std::filesystem::path weixin_account_file(const std::string& hermes_home,
                                          const std::string& account_id) {
    return weixin_account_dir(hermes_home) / (account_id + ".json");
}

static std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void weixin_save_account(const std::string& hermes_home,
                         const WeixinAccountRecord& rec) {
    nlohmann::json payload = {
        {"token", rec.token},
        {"base_url", rec.base_url},
        {"user_id", rec.user_id},
        {"saved_at", rec.saved_at.empty() ? iso_utc_now() : rec.saved_at},
    };
    auto path = weixin_account_file(hermes_home, rec.account_id);
    std::ofstream out(path);
    out << payload.dump(2);
    out.close();
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
#endif
}

std::optional<WeixinAccountRecord> weixin_load_account(
    const std::string& hermes_home, const std::string& account_id) {
    auto path = weixin_account_file(hermes_home, account_id);
    if (!std::filesystem::exists(path)) return std::nullopt;
    try {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        WeixinAccountRecord rec;
        rec.account_id = account_id;
        rec.token = j.value("token", "");
        rec.base_url = j.value("base_url", "");
        rec.user_id = j.value("user_id", "");
        rec.saved_at = j.value("saved_at", "");
        return rec;
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path weixin_sync_buf_path(const std::string& hermes_home,
                                           const std::string& account_id) {
    return weixin_account_dir(hermes_home) / (account_id + ".sync-buf.txt");
}

std::string weixin_load_sync_buf(const std::string& hermes_home,
                                 const std::string& account_id) {
    auto path = weixin_sync_buf_path(hermes_home, account_id);
    if (!std::filesystem::exists(path)) return {};
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void weixin_save_sync_buf(const std::string& hermes_home,
                          const std::string& account_id,
                          const std::string& sync_buf) {
    auto path = weixin_sync_buf_path(hermes_home, account_id);
    std::ofstream out(path);
    out << sync_buf;
}

// ---------------------------------------------------------------------------
// ContextTokenStore + TypingTicketCache
// ---------------------------------------------------------------------------

WeixinContextTokenStore::WeixinContextTokenStore(std::string hermes_home)
    : hermes_home_(std::move(hermes_home)) {}

std::filesystem::path WeixinContextTokenStore::path_for(
    const std::string& account_id) const {
    return weixin_account_dir(hermes_home_) / (account_id + ".context-tokens.json");
}

void WeixinContextTokenStore::restore(const std::string& account_id) {
    auto path = path_for(account_id);
    if (!std::filesystem::exists(path)) return;
    try {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) return;
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                cache_[key(account_id, it.key())] = it.value().get<std::string>();
            }
        }
    } catch (...) {
        // silently ignore
    }
}

std::optional<std::string> WeixinContextTokenStore::get(
    const std::string& account_id, const std::string& user_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key(account_id, user_id));
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

void WeixinContextTokenStore::set(const std::string& account_id,
                                  const std::string& user_id,
                                  const std::string& token) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[key(account_id, user_id)] = token;
    }
    persist(account_id);
}

std::size_t WeixinContextTokenStore::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_.size();
}

void WeixinContextTokenStore::persist(const std::string& account_id) {
    std::string prefix = account_id + ":";
    nlohmann::json payload = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [k, v] : cache_) {
            if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0) {
                payload[k.substr(prefix.size())] = v;
            }
        }
    }
    try {
        std::ofstream out(path_for(account_id));
        out << payload.dump();
    } catch (...) {
        // silently ignore
    }
}

std::optional<std::string> WeixinTypingTicketCache::get(
    const std::string& user_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(user_id);
    if (it == cache_.end()) return std::nullopt;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - it->second.stored).count();
    if (elapsed >= ttl_seconds_) {
        cache_.erase(it);
        return std::nullopt;
    }
    return it->second.ticket;
}

void WeixinTypingTicketCache::set(const std::string& user_id,
                                  const std::string& ticket) {
    std::lock_guard<std::mutex> lk(mu_);
    cache_[user_id] = {ticket, std::chrono::steady_clock::now()};
}

std::size_t WeixinTypingTicketCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_.size();
}

// ---------------------------------------------------------------------------
// QR login phase ser/deser.
// ---------------------------------------------------------------------------

std::string_view to_string(WeixinQrPhase p) {
    switch (p) {
        case WeixinQrPhase::New: return "new";
        case WeixinQrPhase::WaitScan: return "wait_scan";
        case WeixinQrPhase::WaitConfirm: return "wait_confirm";
        case WeixinQrPhase::LoggedIn: return "logged_in";
        case WeixinQrPhase::Cancelled: return "cancelled";
        case WeixinQrPhase::Timeout: return "timeout";
        case WeixinQrPhase::Error: return "error";
    }
    return "unknown";
}

WeixinQrPhase parse_qr_phase(std::string_view s) {
    if (s == "new") return WeixinQrPhase::New;
    if (s == "wait_scan") return WeixinQrPhase::WaitScan;
    if (s == "wait_confirm") return WeixinQrPhase::WaitConfirm;
    if (s == "logged_in") return WeixinQrPhase::LoggedIn;
    if (s == "cancelled") return WeixinQrPhase::Cancelled;
    if (s == "timeout") return WeixinQrPhase::Timeout;
    return WeixinQrPhase::Error;
}

// ---------------------------------------------------------------------------
// WeixinAdapter
// ---------------------------------------------------------------------------

WeixinAdapter::WeixinAdapter(Config cfg)
    : cfg_(std::move(cfg)),
      ctx_tokens_(cfg_.hermes_home),
      typing_tickets_() {
    if (!cfg_.account_id.empty() && !cfg_.hermes_home.empty()) {
        sync_buf_ = weixin_load_sync_buf(cfg_.hermes_home, cfg_.account_id);
        ctx_tokens_.restore(cfg_.account_id);
    }
}

WeixinAdapter::WeixinAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : WeixinAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* WeixinAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WeixinAdapter::connect() {
    last_error_.clear();
    last_error_kind_ = AdapterErrorKind::None;
    if (!cfg_.appid.empty() && !cfg_.appsecret.empty()) {
        if (refresh_access_token()) {
            connected_ = true;
            return true;
        }
    }
    // iLink auth already established externally (token provided).
    if (!cfg_.user_token.empty() && !cfg_.base_url.empty()) {
        connected_ = true;
        return true;
    }
    last_error_kind_ = AdapterErrorKind::Fatal;
    last_error_ = "missing credentials";
    return false;
}

void WeixinAdapter::disconnect() {
    wx_access_token_.clear();
    connected_ = false;
    if (!cfg_.account_id.empty() && !cfg_.hermes_home.empty() && !sync_buf_.empty()) {
        weixin_save_sync_buf(cfg_.hermes_home, cfg_.account_id, sync_buf_);
    }
}

bool WeixinAdapter::refresh_access_token() {
    auto* transport = get_transport();
    if (!transport) return false;
    std::string url =
        "https://api.weixin.qq.com/cgi-bin/token"
        "?grant_type=client_credential&appid=" +
        cfg_.appid + "&secret=" + cfg_.appsecret;
    try {
        auto resp = transport->get(url, {});
        if (resp.status_code != 200) {
            last_error_kind_ = AdapterErrorKind::Retryable;
            last_error_ = "access_token HTTP " + std::to_string(resp.status_code);
            return false;
        }
        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("access_token")) {
            last_error_kind_ = AdapterErrorKind::Fatal;
            last_error_ = "no access_token in response";
            return false;
        }
        wx_access_token_ = body["access_token"].get<std::string>();
        int expires_in = body.value("expires_in", 7200);
        wx_token_expiry_ = std::chrono::steady_clock::now() +
                            std::chrono::seconds(expires_in - 60);
        return true;
    } catch (const std::exception& e) {
        last_error_kind_ = AdapterErrorKind::Retryable;
        last_error_ = e.what();
        return false;
    }
}

bool WeixinAdapter::send_custom_text(const std::string& touser,
                                     const std::string& text) {
    auto* transport = get_transport();
    if (!transport || wx_access_token_.empty()) return false;
    nlohmann::json payload = {
        {"touser", touser},
        {"msgtype", "text"},
        {"text", {{"content", text}}},
    };
    try {
        auto resp = transport->post_json(
            "https://api.weixin.qq.com/cgi-bin/message/custom/send"
            "?access_token=" + wx_access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeixinAdapter::send_custom_typing(const std::string& touser) {
    auto* transport = get_transport();
    if (!transport || wx_access_token_.empty()) return false;
    nlohmann::json payload = {
        {"touser", touser},
        {"command", "Typing"},
    };
    try {
        transport->post_json(
            "https://api.weixin.qq.com/cgi-bin/message/custom/typing"
            "?access_token=" + wx_access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        return true;
    } catch (...) {
        return false;
    }
}

bool WeixinAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    // Chunked delivery.
    auto chunks = split_text(content);
    bool any_fail = false;
    for (const auto& c : chunks) {
        bool ok = false;
        if (!cfg_.appid.empty() && !wx_access_token_.empty()) {
            ok = send_custom_text(chat_id, c);
        } else if (!cfg_.user_token.empty()) {
            ok = ilink_send_text(chat_id, c);
        }
        if (!ok) any_fail = true;
    }
    return !any_fail;
}

void WeixinAdapter::send_typing(const std::string& chat_id) {
    if (!cfg_.appid.empty() && !wx_access_token_.empty()) {
        send_custom_typing(chat_id);
    } else if (!cfg_.user_token.empty()) {
        ilink_send_typing(chat_id, true);
    }
}

std::vector<std::string> WeixinAdapter::split_text(
    const std::string& content) const {
    return weixin_split_text_for_delivery(content, cfg_.max_chunk_chars);
}

// ----- iLink wire helpers ---------------------------------------------------

nlohmann::json WeixinAdapter::ilink_api_post(const std::string& endpoint,
                                             const nlohmann::json& payload) {
    auto* transport = get_transport();
    if (!transport) throw std::runtime_error("no transport");
    std::string base = cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/" + endpoint;
    nlohmann::json full = payload;
    full["base_info"] = {{"channel_version", kChannelVersion}};
    std::string body = weixin_json_dumps(full);
    auto headers = weixin_headers(cfg_.user_token, body);
    auto resp = transport->post_json(url, headers, body);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error("iLink POST " + endpoint + " HTTP " +
                                 std::to_string(resp.status_code));
    }
    return nlohmann::json::parse(resp.body);
}

nlohmann::json WeixinAdapter::ilink_api_get(const std::string& endpoint) {
    auto* transport = get_transport();
    if (!transport) throw std::runtime_error("no transport");
    std::string base = cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/" + endpoint;
    std::unordered_map<std::string, std::string> hdrs = {
        {"iLink-App-Id", kILinkAppId},
        {"iLink-App-ClientVersion", std::to_string(kILinkAppClientVersion)},
    };
    auto resp = transport->get(url, hdrs);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error("iLink GET " + endpoint + " HTTP " +
                                 std::to_string(resp.status_code));
    }
    return nlohmann::json::parse(resp.body);
}

WeixinQrStatus WeixinAdapter::request_qr_code() {
    WeixinQrStatus st;
    st.phase = WeixinQrPhase::New;
    try {
        auto j = ilink_api_post(kEpLoginQrCode, nlohmann::json::object());
        st.qr_url = j.value("qrcode_url", "");
        st.token = j.value("token", "");
        st.base_url = j.value("base_url", cfg_.base_url);
        st.phase = WeixinQrPhase::WaitScan;
        return st;
    } catch (const std::exception& e) {
        st.phase = WeixinQrPhase::Error;
        st.message = e.what();
        return st;
    }
}

WeixinQrStatus WeixinAdapter::poll_qr_status(const std::string& token) {
    WeixinQrStatus st;
    try {
        auto j = ilink_api_post(kEpLoginCheck, {{"token", token}});
        std::string phase = j.value("phase", "wait_scan");
        st.phase = parse_qr_phase(phase);
        st.token = token;
        if (st.phase == WeixinQrPhase::LoggedIn) {
            st.base_url = j.value("base_url", cfg_.base_url);
            st.user_id = j.value("user_id", "");
        }
        return st;
    } catch (const std::exception& e) {
        st.phase = WeixinQrPhase::Error;
        st.message = e.what();
        return st;
    }
}

WeixinAdapter::PollResult WeixinAdapter::poll_once() {
    PollResult r;
    try {
        nlohmann::json payload = {{"get_updates_buf", sync_buf_}};
        auto j = ilink_api_post(kEpGetUpdates, payload);
        r.ok = true;
        r.next_sync_buf = j.value("get_updates_buf", sync_buf_);
        sync_buf_ = r.next_sync_buf;
        if (j.contains("msgs") && j["msgs"].is_array()) {
            for (auto& m : j["msgs"]) r.messages.push_back(m);
        }
        if (!cfg_.account_id.empty() && !cfg_.hermes_home.empty()) {
            weixin_save_sync_buf(cfg_.hermes_home, cfg_.account_id, sync_buf_);
        }
        return r;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
        last_error_kind_ = AdapterErrorKind::Retryable;
        last_error_ = e.what();
        return r;
    }
}

std::string WeixinAdapter::new_client_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return oss.str();
}

bool WeixinAdapter::ilink_send_text(const std::string& to_user,
                                    const std::string& text,
                                    const std::optional<std::string>& context_token) {
    nlohmann::json msg = {
        {"from_user_id", ""},
        {"to_user_id", to_user},
        {"client_id", new_client_id()},
        {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
    };
    if (!text.empty()) {
        msg["item_list"] = nlohmann::json::array(
            {{{"type", kItemText}, {"text_item", {{"text", text}}}}});
    }
    if (context_token && !context_token->empty()) {
        msg["context_token"] = *context_token;
    }
    try {
        auto j = ilink_api_post(kEpSendMessage, {{"msg", msg}});
        return j.value("ret", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeixinAdapter::ilink_fetch_config(std::string* typing_ticket_out,
                                       std::string* cdn_base_url_out) {
    try {
        auto j = ilink_api_post(kEpGetConfig, nlohmann::json::object());
        if (typing_ticket_out) *typing_ticket_out = j.value("typing_ticket", "");
        if (cdn_base_url_out) *cdn_base_url_out = j.value("cdn_base_url", "");
        if (j.contains("cdn_base_url")) cdn_base_url_ = j.value("cdn_base_url", "");
        return true;
    } catch (...) {
        return false;
    }
}

bool WeixinAdapter::ilink_send_typing(const std::string& user_id, bool is_typing) {
    auto ticket = typing_tickets_.get(user_id);
    if (!ticket) {
        std::string new_ticket;
        if (ilink_fetch_config(&new_ticket, nullptr) && !new_ticket.empty()) {
            typing_tickets_.set(user_id, new_ticket);
            ticket = new_ticket;
        }
    }
    if (!ticket) return false;
    try {
        auto j = ilink_api_post(
            kEpSendTyping,
            {{"user_id", user_id}, {"typing", is_typing}, {"ticket", *ticket}});
        return j.value("ret", -1) == 0;
    } catch (...) {
        return false;
    }
}

std::optional<WeixinAdapter::UploadUrlInfo> WeixinAdapter::ilink_get_upload_url(
    const std::string& media_type, const std::string& filename,
    std::size_t file_size) {
    try {
        auto j = ilink_api_post(
            kEpGetUploadUrl,
            {{"media_type", media_type},
             {"filename", filename},
             {"file_size", file_size},
             {"padded_size", weixin_aes_padded_size(file_size)}});
        UploadUrlInfo info;
        info.upload_url = j.value("upload_url", "");
        info.encrypted_param = j.value("encrypted_query_param", "");
        info.filekey = j.value("filekey", "");
        info.aes_key = j.value("aes_key", "");
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

bool WeixinAdapter::ilink_upload_ciphertext(const UploadUrlInfo& info,
                                            const std::string& ciphertext) {
    auto* transport = get_transport();
    if (!transport) return false;
    std::string url = info.upload_url;
    if (url.empty() && !cdn_base_url_.empty()) {
        url = weixin_cdn_upload_url(cdn_base_url_, info.encrypted_param,
                                    info.filekey);
    }
    if (url.empty()) return false;
    try {
        auto resp = transport->post_json(url,
                                          {{"Content-Type", "application/octet-stream"}},
                                          ciphertext);
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

std::string WeixinAdapter::ilink_download_bytes(
    const std::string& encrypted_param, const std::string& aes_key,
    const std::string& cdn_base_url) {
    auto* transport = get_transport();
    if (!transport) return {};
    std::string base = cdn_base_url.empty() ? cdn_base_url_ : cdn_base_url;
    if (base.empty()) return {};
    std::string url = weixin_cdn_download_url(base, encrypted_param);
    try {
        auto resp = transport->get(url, {});
        if (resp.status_code != 200) return {};
        std::string key = weixin_parse_aes_key(aes_key);
        if (key.empty()) return resp.body;
        return weixin_aes128_ecb_decrypt(resp.body, key);
    } catch (...) {
        return {};
    }
}

// ----- Message handling -----------------------------------------------------

bool WeixinAdapter::is_dm_allowed(const std::string& sender_id) const {
    if (cfg_.allowed_dm_senders.empty()) return true;
    return std::find(cfg_.allowed_dm_senders.begin(),
                     cfg_.allowed_dm_senders.end(),
                     sender_id) != cfg_.allowed_dm_senders.end();
}

bool WeixinAdapter::is_group_allowed(const std::string& chat_id,
                                     const std::string& sender_id,
                                     const std::string& text) const {
    bool group_ok = cfg_.allowed_groups.empty() ||
                    std::find(cfg_.allowed_groups.begin(),
                              cfg_.allowed_groups.end(),
                              chat_id) != cfg_.allowed_groups.end();
    if (!group_ok) return false;
    bool sender_ok =
        std::find(cfg_.admin_user_ids.begin(), cfg_.admin_user_ids.end(),
                  sender_id) != cfg_.admin_user_ids.end();
    if (sender_ok) return true;
    if (!cfg_.require_group_mention) return true;
    // Must @mention the bot.
    std::string needle = "@" + cfg_.bot_name;
    return text.find(needle) != std::string::npos;
}

std::optional<MessageEvent> WeixinAdapter::message_from_payload(
    const nlohmann::json& payload) {
    std::string msg_id;
    if (payload.contains("msg_id")) {
        if (payload["msg_id"].is_string()) msg_id = payload["msg_id"].get<std::string>();
        else msg_id = std::to_string(payload.value("msg_id", 0LL));
    }
    if (!msg_id.empty()) {
        std::lock_guard<std::mutex> lk(seen_mu_);
        if (seen_msg_ids_.count(msg_id)) return std::nullopt;
        seen_msg_ids_.insert(msg_id);
        seen_order_.push_back(msg_id);
        while (seen_order_.size() > seen_cap_) {
            seen_msg_ids_.erase(seen_order_.front());
            seen_order_.pop_front();
        }
    }

    WeixinChatKind chat = weixin_guess_chat_type(payload, cfg_.account_id);
    std::string sender = payload.value("from_user_id", "");
    std::string text;
    std::vector<std::string> media_types;
    std::vector<std::string> media_urls;
    if (payload.contains("item_list") && payload["item_list"].is_array()) {
        text = weixin_extract_text(payload["item_list"]);
        for (const auto& it : payload["item_list"]) {
            std::string t = it.value("type", "");
            if (t == kItemImage || t == kItemVideo || t == kItemFile ||
                t == kItemVoice || t == kItemSticker) {
                media_types.push_back(t);
                std::string url;
                if (it.contains(t + "_item")) {
                    const auto& inner = it[t + "_item"];
                    url = inner.value("url", "");
                    if (url.empty()) url = inner.value("cdn_url", "");
                }
                if (!url.empty()) media_urls.push_back(url);
            }
        }
    }

    if (chat.kind == "dm") {
        if (!is_dm_allowed(sender)) return std::nullopt;
    } else {
        if (!is_group_allowed(chat.chat_id, sender, text)) return std::nullopt;
    }

    MessageEvent ev;
    ev.text = text;
    ev.message_type = weixin_message_type_from_media(media_types, text);
    ev.source.platform = Platform::Weixin;
    ev.source.chat_id = chat.chat_id;
    ev.source.chat_type = chat.kind;
    ev.source.user_id = sender;
    ev.media_urls = std::move(media_urls);
    return ev;
}

bool WeixinAdapter::send_image(const std::string& chat_id,
                               const std::string& image_path_or_url) {
    // Wire format: item_list containing image_item with url.
    nlohmann::json msg = {
        {"from_user_id", ""},
        {"to_user_id", chat_id},
        {"client_id", new_client_id()},
        {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
        {"item_list", nlohmann::json::array(
            {{{"type", kItemImage}, {"image_item", {{"url", image_path_or_url}}}}})},
    };
    try {
        auto j = ilink_api_post(kEpSendMessage, {{"msg", msg}});
        return j.value("ret", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeixinAdapter::send_video(const std::string& chat_id,
                               const std::string& video_path) {
    nlohmann::json msg = {
        {"from_user_id", ""}, {"to_user_id", chat_id},
        {"client_id", new_client_id()}, {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
        {"item_list", nlohmann::json::array(
            {{{"type", kItemVideo}, {"video_item", {{"url", video_path}}}}})},
    };
    try {
        return ilink_api_post(kEpSendMessage, {{"msg", msg}}).value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::send_voice(const std::string& chat_id,
                               const std::string& voice_path) {
    nlohmann::json msg = {
        {"from_user_id", ""}, {"to_user_id", chat_id},
        {"client_id", new_client_id()}, {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
        {"item_list", nlohmann::json::array(
            {{{"type", kItemVoice}, {"voice_item", {{"url", voice_path}}}}})},
    };
    try {
        return ilink_api_post(kEpSendMessage, {{"msg", msg}}).value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::send_document(const std::string& chat_id,
                                  const std::string& file_path,
                                  const std::string& caption) {
    nlohmann::json item = {{"type", kItemFile},
                           {"file_item", {{"url", file_path},
                                          {"filename", file_path}}}};
    nlohmann::json items = nlohmann::json::array({item});
    if (!caption.empty()) {
        items.push_back({{"type", kItemText}, {"text_item", {{"text", caption}}}});
    }
    nlohmann::json msg = {
        {"from_user_id", ""}, {"to_user_id", chat_id},
        {"client_id", new_client_id()}, {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
        {"item_list", items},
    };
    try {
        return ilink_api_post(kEpSendMessage, {{"msg", msg}}).value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::forward_message(const std::string& chat_id,
                                    const std::string& source_msg_id) {
    nlohmann::json payload = {
        {"msg_id", source_msg_id},
        {"to_user_id", chat_id},
        {"client_id", new_client_id()},
    };
    try {
        return ilink_api_post("forwardmessage", payload).value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::quote_message(const std::string& chat_id,
                                  const std::string& quoted_msg_id,
                                  const std::string& reply_text) {
    nlohmann::json msg = {
        {"from_user_id", ""}, {"to_user_id", chat_id},
        {"client_id", new_client_id()}, {"message_type", kMsgTypeBot},
        {"message_state", kMsgStateFinish},
        {"item_list", nlohmann::json::array(
            {{{"type", kItemQuote},
              {"quote_item", {{"msg_id", quoted_msg_id},
                              {"text", reply_text}}}}})},
    };
    try {
        return ilink_api_post(kEpSendMessage, {{"msg", msg}}).value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::recall_message(const std::string& chat_id,
                                   const std::string& msg_id) {
    try {
        return ilink_api_post("recallmessage",
                              {{"msg_id", msg_id}, {"to_user_id", chat_id}})
            .value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::send_friend_request(const std::string& user_id,
                                        const std::string& greeting) {
    try {
        return ilink_api_post("friendrequest",
                              {{"to_user_id", user_id}, {"greeting", greeting}})
            .value("ret", -1) == 0;
    } catch (...) { return false; }
}

bool WeixinAdapter::accept_friend_request(const std::string& request_id) {
    try {
        return ilink_api_post("acceptfriendrequest", {{"request_id", request_id}})
            .value("ret", -1) == 0;
    } catch (...) { return false; }
}

// ----- Static XML parser (official-account callback) -----------------------

WeixinMessageEvent WeixinAdapter::parse_xml_message(const std::string& xml) {
    WeixinMessageEvent evt;
    evt.from_user = weixin_extract_xml_tag(xml, "FromUserName");
    evt.to_user = weixin_extract_xml_tag(xml, "ToUserName");
    evt.msg_type = weixin_extract_xml_tag(xml, "MsgType");
    evt.content = weixin_extract_xml_tag(xml, "Content");
    evt.msg_id = weixin_extract_xml_tag(xml, "MsgId");
    evt.event = weixin_extract_xml_tag(xml, "Event");
    evt.event_key = weixin_extract_xml_tag(xml, "EventKey");
    evt.pic_url = weixin_extract_xml_tag(xml, "PicUrl");
    evt.media_id = weixin_extract_xml_tag(xml, "MediaId");
    evt.format = weixin_extract_xml_tag(xml, "Format");
    evt.location_x = weixin_extract_xml_tag(xml, "Location_X");
    evt.location_y = weixin_extract_xml_tag(xml, "Location_Y");
    evt.title = weixin_extract_xml_tag(xml, "Title");
    evt.url = weixin_extract_xml_tag(xml, "Url");
    return evt;
}

}  // namespace hermes::gateway::platforms
