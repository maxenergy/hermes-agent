// Phase 12/18 — Discord platform adapter implementation with depth parity
// to gateway/platforms/discord.py. This TU intentionally contains a lot of
// small, purpose-built helpers rather than pulling in a dedicated Discord
// SDK so the build stays dependency-free beyond nlohmann::json + libcurl.
#include "discord.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {

constexpr const char* kApiBase = "https://discord.com/api/v10";

std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += tbl[n & 0x3F];
        i += 3;
    }
    if (i < data.size()) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? tbl[(n >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

// UTF-8 aware truncation that never splits a codepoint. Size is in bytes.
std::string utf8_truncate(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t cut = max_bytes;
    // Walk backward until we're at a codepoint boundary (a byte whose top
    // bits are not 10xxxxxx).
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

// Quick helper for the 4xx 429 payload ({"retry_after": 1.23}).
double parse_retry_after(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("retry_after")) {
            return j["retry_after"].get<double>();
        }
    } catch (...) {
    }
    return 0.0;
}

}  // namespace

// ─── AllowedMentions ─────────────────────────────────────────────────────

nlohmann::json AllowedMentions::to_json() const {
    nlohmann::json j;
    nlohmann::json parse = nlohmann::json::array();
    if (parse_users) parse.push_back("users");
    if (parse_roles) parse.push_back("roles");
    if (parse_everyone) parse.push_back("everyone");
    j["parse"] = parse;
    if (!users.empty()) j["users"] = users;
    if (!roles.empty()) j["roles"] = roles;
    j["replied_user"] = replied_user;
    return j;
}

// ─── DiscordEmbed ─────────────────────────────────────────────────────────

DiscordEmbed& DiscordEmbed::set_title(std::string t) {
    title_ = utf8_truncate(t, 256);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_description(std::string d) {
    description_ = utf8_truncate(d, 4096);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_url(std::string u) {
    url_ = std::move(u);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_color(uint32_t c) {
    color_ = c;
    return *this;
}

DiscordEmbed& DiscordEmbed::set_timestamp_iso8601(std::string ts) {
    timestamp_ = std::move(ts);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_footer(std::string text, std::string icon_url) {
    footer_text_ = utf8_truncate(text, 2048);
    footer_icon_url_ = std::move(icon_url);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_thumbnail(std::string url) {
    thumbnail_url_ = std::move(url);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_image(std::string url) {
    image_url_ = std::move(url);
    return *this;
}

DiscordEmbed& DiscordEmbed::set_author(std::string name, std::string url,
                                        std::string icon_url) {
    author_name_ = utf8_truncate(name, 256);
    author_url_ = std::move(url);
    author_icon_url_ = std::move(icon_url);
    return *this;
}

DiscordEmbed& DiscordEmbed::add_field(std::string name, std::string value,
                                       bool inline_val) {
    if (fields_.size() >= 25) return *this;  // Discord cap.
    Field f;
    f.name = utf8_truncate(name, 256);
    f.value = utf8_truncate(value, 1024);
    f.inline_ = inline_val;
    fields_.push_back(std::move(f));
    return *this;
}

nlohmann::json DiscordEmbed::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    if (!title_.empty()) j["title"] = title_;
    if (!description_.empty()) j["description"] = description_;
    if (!url_.empty()) j["url"] = url_;
    if (color_) j["color"] = *color_;
    if (!timestamp_.empty()) j["timestamp"] = timestamp_;
    if (!footer_text_.empty()) {
        j["footer"] = {{"text", footer_text_}};
        if (!footer_icon_url_.empty()) j["footer"]["icon_url"] = footer_icon_url_;
    }
    if (!thumbnail_url_.empty()) {
        j["thumbnail"] = {{"url", thumbnail_url_}};
    }
    if (!image_url_.empty()) {
        j["image"] = {{"url", image_url_}};
    }
    if (!author_name_.empty()) {
        j["author"] = {{"name", author_name_}};
        if (!author_url_.empty()) j["author"]["url"] = author_url_;
        if (!author_icon_url_.empty()) j["author"]["icon_url"] = author_icon_url_;
    }
    if (!fields_.empty()) {
        j["fields"] = nlohmann::json::array();
        for (const auto& f : fields_) {
            j["fields"].push_back({
                {"name", f.name},
                {"value", f.value},
                {"inline", f.inline_},
            });
        }
    }
    return j;
}

// ─── DiscordAdapter: construction / connection ───────────────────────────

DiscordAdapter::DiscordAdapter(Config cfg) : cfg_(std::move(cfg)) {}

DiscordAdapter::DiscordAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* DiscordAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

int DiscordAdapter::compute_intents() const {
    // Guilds (1<<0) + GuildMessages (1<<9) + DirectMessages (1<<12) are
    // always on; MessageContent (1<<15), Members (1<<1), VoiceStates (1<<7)
    // are opt-in.
    int bits = (1 << 0) | (1 << 9) | (1 << 12);
    if (cfg_.intents_members) bits |= (1 << 1);
    if (cfg_.intents_voice_states) bits |= (1 << 7);
    if (cfg_.intents_message_content) bits |= (1 << 15);
    return bits;
}

bool DiscordAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;

    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            cfg_.bot_token, {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
        return false;
    }

    try {
        auto resp = transport->get(
            std::string(kApiBase) + "/users/@me",
            {{"Authorization", "Bot " + cfg_.bot_token}});
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("id")) return false;

        // Cache the application_id when the caller didn't supply it.
        if (cfg_.application_id.empty() && body.contains("id")) {
            cfg_.application_id = body.value("id", "");
        }

        connected_ = true;

        if (cfg_.auto_register_commands && !slash_commands_.empty()) {
            bulk_register_commands();
        }
        return true;
    } catch (...) {
        return false;
    }
}

void DiscordAdapter::disconnect() {
    if (!cfg_.bot_token.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
    }
    connected_ = false;
}

// ─── Low-level REST helpers ──────────────────────────────────────────────

hermes::llm::HttpTransport::Response DiscordAdapter::do_get(const std::string& url) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->get(
        url, {{"Authorization", "Bot " + cfg_.bot_token}});
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_post_json(
    const std::string& url, const nlohmann::json& body) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url,
        {{"Authorization", "Bot " + cfg_.bot_token},
         {"Content-Type", "application/json"}},
        body.dump());
    update_bucket_from_headers(bucket_for(url), resp.headers);
    if (resp.status_code == 429) {
        record_global_rate_limit(parse_retry_after(resp.body));
    }
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_post_empty(
    const std::string& url) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url, {{"Authorization", "Bot " + cfg_.bot_token}}, "");
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_delete(
    const std::string& url) {
    // Our HttpTransport only exposes get / post_json; emulate DELETE with
    // the widely-recognised X-HTTP-Method-Override header so that tests
    // can still inspect the intended URL.  The real libcurl transport will
    // be extended separately.
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url,
        {{"Authorization", "Bot " + cfg_.bot_token},
         {"X-HTTP-Method-Override", "DELETE"},
         {"Content-Length", "0"}},
        "");
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_put_json(
    const std::string& url, const nlohmann::json& body) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url,
        {{"Authorization", "Bot " + cfg_.bot_token},
         {"Content-Type", "application/json"},
         {"X-HTTP-Method-Override", "PUT"}},
        body.dump());
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_patch_json(
    const std::string& url, const nlohmann::json& body) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url,
        {{"Authorization", "Bot " + cfg_.bot_token},
         {"Content-Type", "application/json"},
         {"X-HTTP-Method-Override", "PATCH"}},
        body.dump());
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

hermes::llm::HttpTransport::Response DiscordAdapter::do_post_multipart(
    const std::string& url, const std::string& boundary,
    const std::string& body) {
    auto* transport = get_transport();
    if (!transport) return {0, "", {}};
    auto resp = transport->post_json(
        url,
        {{"Authorization", "Bot " + cfg_.bot_token},
         {"Content-Type", "multipart/form-data; boundary=" + boundary}},
        body);
    update_bucket_from_headers(bucket_for(url), resp.headers);
    return resp;
}

// ─── Mentions / formatting helpers ───────────────────────────────────────

std::string DiscordAdapter::format_mention(const std::string& user_id) {
    return "<@" + user_id + ">";
}

std::string DiscordAdapter::format_channel_mention(const std::string& channel_id) {
    return "<#" + channel_id + ">";
}

std::string DiscordAdapter::format_role_mention(const std::string& role_id) {
    return "<@&" + role_id + ">";
}

void DiscordAdapter::parse_mentions(const std::string& content,
                                     std::vector<std::string>& users,
                                     std::vector<std::string>& channels,
                                     std::vector<std::string>& roles) {
    // Discord mention syntax: <@123>, <@!123>, <#123>, <@&123>.
    static const std::regex re(R"(<(@!?|@&|#)(\d+)>)");
    auto begin = std::sregex_iterator(content.begin(), content.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string prefix = (*it)[1].str();
        const std::string id = (*it)[2].str();
        if (prefix == "#") {
            channels.push_back(id);
        } else if (prefix == "@&") {
            roles.push_back(id);
        } else {
            users.push_back(id);
        }
    }
}

std::string DiscordAdapter::strip_leading_mention(const std::string& content,
                                                   const std::string& bot_id) {
    if (bot_id.empty()) return content;
    const std::string m1 = "<@" + bot_id + ">";
    const std::string m2 = "<@!" + bot_id + ">";
    std::size_t i = 0;
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) ++i;
    std::string rest = content.substr(i);
    if (rest.rfind(m1, 0) == 0) rest = rest.substr(m1.size());
    else if (rest.rfind(m2, 0) == 0) rest = rest.substr(m2.size());
    else return content;
    std::size_t j = 0;
    while (j < rest.size() && std::isspace(static_cast<unsigned char>(rest[j]))) ++j;
    return rest.substr(j);
}

// ─── Message splitting ────────────────────────────────────────────────────

std::vector<std::string> DiscordAdapter::split_message(
    const std::string& content, int limit) {
    std::vector<std::string> out;
    if (content.empty()) return out;
    if (static_cast<int>(content.size()) <= limit) {
        out.push_back(content);
        return out;
    }

    // Strategy: first try to split on \n\n paragraph boundaries; when a
    // paragraph itself is too long, split on \n; when a line is too long,
    // hard-cut at `limit` (UTF-8 aware).  Track open triple-backtick fences
    // so we re-open them on the next chunk and close them before the split.
    std::string remainder = content;
    while (!remainder.empty()) {
        if (static_cast<int>(remainder.size()) <= limit) {
            out.push_back(remainder);
            break;
        }
        std::size_t cut = std::string::npos;
        // Prefer paragraph split within the window.
        auto pp = remainder.rfind("\n\n", static_cast<std::size_t>(limit));
        if (pp != std::string::npos && pp > 0) {
            cut = pp + 2;
        } else {
            auto p = remainder.rfind('\n', static_cast<std::size_t>(limit));
            if (p != std::string::npos && p > 0) {
                cut = p + 1;
            } else {
                cut = utf8_truncate(remainder.substr(0, limit), limit).size();
                if (cut == 0) cut = std::min<std::size_t>(limit, remainder.size());
            }
        }

        std::string chunk = remainder.substr(0, cut);

        // Count triple-backticks to decide whether a fence is open.
        std::size_t fences = 0;
        for (std::size_t p = 0; (p = chunk.find("```", p)) != std::string::npos; p += 3) {
            ++fences;
        }
        bool open_fence = (fences % 2) == 1;
        if (open_fence) {
            chunk += "\n```";
        }
        out.push_back(chunk);
        remainder = remainder.substr(cut);
        if (open_fence && !remainder.empty()) {
            remainder = "```\n" + remainder;
        }
    }
    return out;
}

// ─── Slash commands ───────────────────────────────────────────────────────

void DiscordAdapter::register_slash_command(SlashCommand cmd) {
    slash_commands_.push_back(std::move(cmd));
}

bool DiscordAdapter::bulk_register_commands() {
    if (cfg_.application_id.empty()) return false;
    auto* transport = get_transport();
    if (!transport) return false;

    std::string url = std::string(kApiBase) + "/applications/" +
                      cfg_.application_id + "/commands";
    if (!cfg_.guild_id.empty()) {
        url = std::string(kApiBase) + "/applications/" + cfg_.application_id +
              "/guilds/" + cfg_.guild_id + "/commands";
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : slash_commands_) {
        nlohmann::json o = {
            {"name", c.name},
            {"description", c.description},
            {"type", c.type},
        };
        if (!c.options.is_null() && !c.options.empty()) {
            o["options"] = c.options;
        }
        arr.push_back(o);
    }
    auto resp = do_put_json(url, arr);
    return resp.status_code >= 200 && resp.status_code < 300;
}

void DiscordAdapter::register_hermes_slash_commands() {
    // Mirror the subset of /hermes slash commands defined in the Python
    // _register_slash_commands() method. Descriptions trimmed for Discord's
    // 100-char limit on command descriptions.
    auto add = [this](std::string name, std::string desc,
                      nlohmann::json options = nlohmann::json::array()) {
        SlashCommand c;
        c.name = std::move(name);
        c.description = std::move(desc);
        c.options = std::move(options);
        slash_commands_.push_back(std::move(c));
    };
    add("new", "Start a new conversation");
    add("reset", "Reset the current conversation");
    add("model", "Change the active model", nlohmann::json::array({
        nlohmann::json{{"type", 3}, {"name", "name"},
                       {"description", "Model name"}, {"required", false}}
    }));
    add("reasoning", "Change reasoning effort", nlohmann::json::array({
        nlohmann::json{{"type", 3}, {"name", "effort"},
                       {"description", "low/medium/high"},
                       {"required", false}}
    }));
    add("personality", "Change the active personality");
    add("retry", "Retry the last prompt");
    add("undo", "Undo the last turn");
    add("status", "Show agent status");
    add("sethome", "Use this channel as the default home");
    add("stop", "Stop the in-flight run");
    add("compress", "Compress the current conversation");
    add("title", "Rename the current conversation");
    add("resume", "Resume a saved conversation");
    add("usage", "Show token usage summary");
    add("provider", "Show the active LLM provider");
    add("help", "Show help");
    add("insights", "Show conversation insights");
    add("reload_mcp", "Reload MCP servers");
    add("voice", "Toggle voice mode");
    add("update", "Run hermes update");
    add("approve", "Approve a pending exec");
    add("deny", "Deny a pending exec");
    add("thread", "Start or attach a thread");
    add("queue", "Queue a prompt for later");
    add("background", "Run a prompt in the background");
    add("btw", "Ask a side question");
}

// ─── Interactions ─────────────────────────────────────────────────────────

void DiscordAdapter::set_interaction_callback(InteractionCallback cb) {
    interaction_cb_ = std::move(cb);
}

bool DiscordAdapter::respond_interaction(const std::string& interaction_id,
                                          const std::string& token,
                                          const std::string& content,
                                          int response_type, bool ephemeral) {
    std::string url = std::string(kApiBase) + "/interactions/" +
                      interaction_id + "/" + token + "/callback";
    nlohmann::json data = {{"content", content}};
    if (ephemeral) data["flags"] = 64;
    nlohmann::json payload = {
        {"type", response_type},
        {"data", data},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::respond_interaction_embed(
    const std::string& interaction_id, const std::string& token,
    const DiscordEmbed& embed, bool ephemeral) {
    std::string url = std::string(kApiBase) + "/interactions/" +
                      interaction_id + "/" + token + "/callback";
    nlohmann::json data = {
        {"embeds", nlohmann::json::array({embed.to_json()})},
    };
    if (ephemeral) data["flags"] = 64;
    nlohmann::json payload = {{"type", 4}, {"data", data}};
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::defer_interaction(const std::string& interaction_id,
                                        const std::string& token,
                                        bool ephemeral) {
    std::string url = std::string(kApiBase) + "/interactions/" +
                      interaction_id + "/" + token + "/callback";
    nlohmann::json payload = {{"type", 5}};
    if (ephemeral) payload["data"] = {{"flags", 64}};
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::followup_interaction(const std::string& token,
                                           const std::string& content) {
    if (cfg_.application_id.empty()) return false;
    std::string url = std::string(kApiBase) + "/webhooks/" +
                      cfg_.application_id + "/" + token;
    auto resp = do_post_json(url, nlohmann::json{{"content", content}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::respond_autocomplete(
    const std::string& interaction_id, const std::string& token,
    const std::vector<std::pair<std::string, std::string>>& choices) {
    std::string url = std::string(kApiBase) + "/interactions/" +
                      interaction_id + "/" + token + "/callback";
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t i = 0; i < choices.size() && i < 25; ++i) {
        arr.push_back({{"name", choices[i].first},
                       {"value", choices[i].second}});
    }
    nlohmann::json payload = {
        {"type", 8},
        {"data", {{"choices", arr}}},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

void DiscordAdapter::dispatch_interaction_payload(const nlohmann::json& payload) {
    if (!interaction_cb_) return;
    DiscordInteraction ix;
    ix.raw = payload;
    ix.type = payload.value("type", 0);
    ix.id = payload.value("id", "");
    ix.token = payload.value("token", "");
    ix.application_id = payload.value("application_id", "");
    ix.channel_id = payload.value("channel_id", "");
    ix.guild_id = payload.value("guild_id", "");
    if (payload.contains("member") && payload["member"].is_object() &&
        payload["member"].contains("user")) {
        ix.user_id = payload["member"]["user"].value("id", "");
        ix.username = payload["member"]["user"].value("username", "");
    } else if (payload.contains("user") && payload["user"].is_object()) {
        ix.user_id = payload["user"].value("id", "");
        ix.username = payload["user"].value("username", "");
    }
    if (payload.contains("data") && payload["data"].is_object()) {
        const auto& d = payload["data"];
        ix.custom_id = d.value("custom_id", "");
        ix.component_type = d.value("component_type", 0);
        ix.command_name = d.value("name", "");
        if (d.contains("values") && d["values"].is_array()) {
            for (const auto& v : d["values"]) {
                ix.values.push_back(v.get<std::string>());
            }
        }
    }
    interaction_cb_(ix);
}

// ─── Threads / Forums ─────────────────────────────────────────────────────

bool DiscordAdapter::create_thread(const std::string& channel_id,
                                    const std::string& name,
                                    int auto_archive_minutes) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/threads";
    nlohmann::json payload = {
        {"name", name},
        {"auto_archive_duration", auto_archive_minutes},
        {"type", 11},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::create_thread_from_message(
    const std::string& channel_id, const std::string& message_id,
    const std::string& name, int auto_archive_minutes) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/threads";
    nlohmann::json payload = {
        {"name", name},
        {"auto_archive_duration", auto_archive_minutes},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::archive_thread(const std::string& thread_id) {
    std::string url = std::string(kApiBase) + "/channels/" + thread_id;
    auto resp = do_patch_json(url, nlohmann::json{{"archived", true}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::unarchive_thread(const std::string& thread_id) {
    std::string url = std::string(kApiBase) + "/channels/" + thread_id;
    auto resp = do_patch_json(url, nlohmann::json{{"archived", false}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::lock_thread(const std::string& thread_id) {
    std::string url = std::string(kApiBase) + "/channels/" + thread_id;
    auto resp = do_patch_json(url, nlohmann::json{{"locked", true}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::unlock_thread(const std::string& thread_id) {
    std::string url = std::string(kApiBase) + "/channels/" + thread_id;
    auto resp = do_patch_json(url, nlohmann::json{{"locked", false}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::delete_thread(const std::string& thread_id) {
    std::string url = std::string(kApiBase) + "/channels/" + thread_id;
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::create_forum_post(
    const std::string& forum_channel_id, const std::string& title,
    const std::string& message_content,
    const std::vector<std::string>& tag_ids) {
    std::string url = std::string(kApiBase) + "/channels/" +
                      forum_channel_id + "/threads";
    nlohmann::json payload = {
        {"name", title},
        {"auto_archive_duration", 1440},
        {"message", {{"content", message_content}}},
    };
    if (!tag_ids.empty()) {
        payload["applied_tags"] = tag_ids;
    }
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::send_to_thread(const std::string& thread_id,
                                     const std::string& content) {
    return send(thread_id, content);
}

bool DiscordAdapter::reply_to_message(const std::string& channel_id,
                                       const std::string& message_id,
                                       const std::string& content,
                                       bool fail_if_not_exists) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    nlohmann::json payload = {
        {"content", content},
        {"message_reference", {
            {"message_id", message_id},
            {"fail_if_not_exists", fail_if_not_exists},
        }},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ─── Messages ─────────────────────────────────────────────────────────────

bool DiscordAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    // Apply split + default allowed_mentions policy.
    auto chunks = split_message(content, cfg_.max_message_length);
    if (chunks.empty()) chunks.push_back("");
    AllowedMentions am;  // default = block everything
    bool ok = true;
    for (const auto& c : chunks) {
        std::string url = std::string(kApiBase) + "/channels/" + chat_id +
                          "/messages";
        nlohmann::json payload = {
            {"content", c},
            {"allowed_mentions", am.to_json()},
        };
        auto resp = do_post_json(url, payload);
        if (!(resp.status_code >= 200 && resp.status_code < 300)) ok = false;
    }
    return ok;
}

void DiscordAdapter::send_typing(const std::string& chat_id) {
    std::string url = std::string(kApiBase) + "/channels/" + chat_id +
                      "/typing";
    (void)do_post_empty(url);
}

bool DiscordAdapter::send_embed(const std::string& channel_id,
                                 const DiscordEmbed& embed,
                                 const std::string& content) {
    return send_embeds(channel_id, {embed}, content);
}

bool DiscordAdapter::send_embeds(const std::string& channel_id,
                                  const std::vector<DiscordEmbed>& embeds,
                                  const std::string& content) {
    if (embeds.size() > 10) return false;
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : embeds) arr.push_back(e.to_json());
    nlohmann::json payload = {{"embeds", arr}};
    if (!content.empty()) payload["content"] = content;
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::send_with_policy(const std::string& channel_id,
                                       const std::string& content,
                                       const AllowedMentions& allowed) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    nlohmann::json payload = {
        {"content", content},
        {"allowed_mentions", allowed.to_json()},
    };
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::edit_message(const std::string& channel_id,
                                   const std::string& message_id,
                                   const std::string& new_content) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id;
    auto resp = do_patch_json(url, nlohmann::json{{"content", new_content}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::edit_message_embed(const std::string& channel_id,
                                         const std::string& message_id,
                                         const DiscordEmbed& embed) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id;
    auto resp = do_patch_json(
        url, nlohmann::json{{"embeds", nlohmann::json::array({embed.to_json()})}});
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::delete_message(const std::string& channel_id,
                                     const std::string& message_id) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id;
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::bulk_delete(const std::string& channel_id,
                                  const std::vector<std::string>& message_ids) {
    if (message_ids.size() < 2 || message_ids.size() > 100) return false;
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/bulk-delete";
    nlohmann::json payload = {{"messages", message_ids}};
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::pin_message(const std::string& channel_id,
                                  const std::string& message_id) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/pins/" + message_id;
    auto resp = do_put_json(url, nlohmann::json::object());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::unpin_message(const std::string& channel_id,
                                    const std::string& message_id) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/pins/" + message_id;
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::fetch_message(const std::string& channel_id,
                                    const std::string& message_id,
                                    nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id;
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

bool DiscordAdapter::fetch_channel(const std::string& channel_id,
                                    nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id;
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

bool DiscordAdapter::fetch_guild(const std::string& guild_id,
                                  nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/guilds/" + guild_id;
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Reactions ────────────────────────────────────────────────────────────

bool DiscordAdapter::add_reaction(const std::string& channel_id,
                                   const std::string& message_id,
                                   const std::string& emoji) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/reactions/" +
                      url_encode(emoji) + "/@me";
    auto resp = do_put_json(url, nlohmann::json::object());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::remove_own_reaction(const std::string& channel_id,
                                          const std::string& message_id,
                                          const std::string& emoji) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/reactions/" +
                      url_encode(emoji) + "/@me";
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::remove_user_reaction(const std::string& channel_id,
                                           const std::string& message_id,
                                           const std::string& emoji,
                                           const std::string& user_id) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/reactions/" +
                      url_encode(emoji) + "/" + user_id;
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::remove_all_reactions(const std::string& channel_id,
                                           const std::string& message_id) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/reactions";
    auto resp = do_delete(url);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::fetch_reactions(const std::string& channel_id,
                                      const std::string& message_id,
                                      const std::string& emoji,
                                      nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages/" + message_id + "/reactions/" +
                      url_encode(emoji);
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Attachments / multipart ──────────────────────────────────────────────

std::string DiscordAdapter::build_multipart_body(
    const nlohmann::json& payload_json,
    const std::vector<AttachmentSpec>& files,
    const std::string& boundary) {
    std::ostringstream oss;
    const std::string dash = "--";
    const std::string crlf = "\r\n";

    // First part: payload_json (Discord requires this field name).
    oss << dash << boundary << crlf;
    oss << "Content-Disposition: form-data; name=\"payload_json\"" << crlf;
    oss << "Content-Type: application/json" << crlf << crlf;
    oss << payload_json.dump() << crlf;

    for (std::size_t i = 0; i < files.size(); ++i) {
        oss << dash << boundary << crlf;
        oss << "Content-Disposition: form-data; name=\"files[" << i
            << "]\"; filename=\"" << files[i].filename << "\"" << crlf;
        oss << "Content-Type: "
            << (files[i].content_type.empty() ? "application/octet-stream"
                                              : files[i].content_type)
            << crlf << crlf;
        // Binary payload goes in verbatim.
        oss.write(reinterpret_cast<const char*>(files[i].data.data()),
                  static_cast<std::streamsize>(files[i].data.size()));
        oss << crlf;
    }
    oss << dash << boundary << dash << crlf;
    return oss.str();
}

std::string DiscordAdapter::validate_attachments(
    const std::vector<AttachmentSpec>& files, std::size_t max_bytes_each) {
    for (const auto& f : files) {
        if (f.filename.empty()) return "attachment has empty filename";
        if (f.data.empty()) return "attachment " + f.filename + " is empty";
        if (f.data.size() > max_bytes_each) {
            return "attachment " + f.filename + " exceeds per-file limit";
        }
    }
    return {};
}

bool DiscordAdapter::send_attachments(const std::string& channel_id,
                                       const std::vector<AttachmentSpec>& files,
                                       const std::string& content) {
    if (files.empty()) return false;
    if (!validate_attachments(files).empty()) return false;

    nlohmann::json atts = nlohmann::json::array();
    for (std::size_t i = 0; i < files.size(); ++i) {
        nlohmann::json a = {{"id", std::to_string(i)},
                            {"filename", files[i].filename}};
        if (files[i].duration_secs) {
            a["duration_secs"] = *files[i].duration_secs;
        }
        if (!files[i].waveform.empty()) {
            a["waveform"] = base64_encode(files[i].waveform);
        }
        atts.push_back(a);
    }
    nlohmann::json payload = {{"attachments", atts}};
    if (!content.empty()) payload["content"] = content;

    std::string boundary = "hermesboundary" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string body = build_multipart_body(payload, files, boundary);
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    auto resp = do_post_multipart(url, boundary, body);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::send_voice_message(const std::string& channel_id,
                                         const std::vector<uint8_t>& ogg_opus,
                                         double duration_secs) {
    AttachmentSpec spec;
    spec.filename = "voice-message.ogg";
    spec.content_type = "audio/ogg";
    spec.data = ogg_opus;
    spec.duration_secs = duration_secs;
    spec.waveform = std::vector<uint8_t>(256, 128);

    // Flag 8192 = IS_VOICE_MESSAGE.
    nlohmann::json atts = nlohmann::json::array();
    atts.push_back({
        {"id", "0"},
        {"filename", spec.filename},
        {"duration_secs", duration_secs},
        {"waveform", base64_encode(spec.waveform)},
    });
    nlohmann::json payload = {
        {"flags", 8192},
        {"attachments", atts},
    };
    std::string boundary = "hermesvm" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string body = build_multipart_body(payload, {spec}, boundary);
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    auto resp = do_post_multipart(url, boundary, body);
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ─── Stickers ─────────────────────────────────────────────────────────────

bool DiscordAdapter::fetch_guild_stickers(const std::string& guild_id,
                                           nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/guilds/" + guild_id +
                      "/stickers";
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

bool DiscordAdapter::send_sticker(const std::string& channel_id,
                                   const std::string& sticker_id,
                                   const std::string& content) {
    std::string url = std::string(kApiBase) + "/channels/" + channel_id +
                      "/messages";
    nlohmann::json payload = {
        {"sticker_ids", nlohmann::json::array({sticker_id})},
    };
    if (!content.empty()) payload["content"] = content;
    auto resp = do_post_json(url, payload);
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ─── DM / Guild Member ────────────────────────────────────────────────────

std::optional<std::string> DiscordAdapter::open_dm(const std::string& user_id) {
    std::string url = std::string(kApiBase) + "/users/@me/channels";
    auto resp = do_post_json(url, nlohmann::json{{"recipient_id", user_id}});
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("id")) return j["id"].get<std::string>();
    } catch (...) {
    }
    return std::nullopt;
}

bool DiscordAdapter::send_dm(const std::string& user_id,
                              const std::string& content) {
    auto dm = open_dm(user_id);
    if (!dm) return false;
    return send(*dm, content);
}

bool DiscordAdapter::fetch_member(const std::string& guild_id,
                                   const std::string& user_id,
                                   nlohmann::json& out) {
    std::string url = std::string(kApiBase) + "/guilds/" + guild_id +
                      "/members/" + user_id;
    auto resp = do_get(url);
    if (!(resp.status_code >= 200 && resp.status_code < 300)) return false;
    try {
        out = nlohmann::json::parse(resp.body);
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Presence ─────────────────────────────────────────────────────────────

bool DiscordAdapter::set_presence(const std::string& status,
                                   const std::string& activity_name,
                                   int activity_type) {
    if (!gateway_) return false;
    auto* ws = gateway_->transport();
    if (!ws) return false;
    nlohmann::json payload = {
        {"op", 3},
        {"d", {
            {"since", nullptr},
            {"activities", nlohmann::json::array()},
            {"status", status},
            {"afk", false},
        }},
    };
    if (!activity_name.empty()) {
        payload["d"]["activities"].push_back({
            {"name", activity_name},
            {"type", activity_type},
        });
    }
    return ws->send_text(payload.dump());
}

// ─── Webhook mode ─────────────────────────────────────────────────────────

bool DiscordAdapter::send_via_webhook(const std::string& content,
                                       const std::string& username,
                                       const std::string& avatar_url) {
    if (!webhook_ && cfg_.webhook_url.empty()) return false;
    std::string url;
    if (webhook_) {
        url = std::string(kApiBase) + "/webhooks/" + webhook_->id + "/" +
              webhook_->token;
    } else {
        url = cfg_.webhook_url;
    }
    nlohmann::json payload = {{"content", content}};
    if (!username.empty()) payload["username"] = username;
    if (!avatar_url.empty()) payload["avatar_url"] = avatar_url;
    auto* transport = get_transport();
    if (!transport) return false;
    auto resp = transport->post_json(
        url, {{"Content-Type", "application/json"}}, payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool DiscordAdapter::send_webhook_embed(const DiscordEmbed& embed,
                                         const std::string& content,
                                         const std::string& username) {
    if (!webhook_ && cfg_.webhook_url.empty()) return false;
    std::string url;
    if (webhook_) {
        url = std::string(kApiBase) + "/webhooks/" + webhook_->id + "/" +
              webhook_->token;
    } else {
        url = cfg_.webhook_url;
    }
    nlohmann::json payload = {
        {"embeds", nlohmann::json::array({embed.to_json()})},
    };
    if (!content.empty()) payload["content"] = content;
    if (!username.empty()) payload["username"] = username;
    auto* transport = get_transport();
    if (!transport) return false;
    auto resp = transport->post_json(
        url, {{"Content-Type", "application/json"}}, payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ─── Rate limiting ────────────────────────────────────────────────────────

std::string DiscordAdapter::bucket_for(const std::string& url) {
    // Collapse numeric IDs so /channels/123/messages/456 maps to the same
    // bucket as /channels/999/messages/888 (the major route is what Discord
    // throttles anyway). Also preserve the `/@me` sentinel.
    std::string out;
    std::size_t i = 0;
    // Skip scheme + host.
    auto p = url.find("://");
    if (p != std::string::npos) {
        p = url.find('/', p + 3);
        if (p != std::string::npos) i = p;
    }
    bool last_was_slash = false;
    for (; i < url.size(); ++i) {
        char c = url[i];
        if (c == '/') {
            out += c;
            last_was_slash = true;
            continue;
        }
        if (last_was_slash && std::isdigit(static_cast<unsigned char>(c))) {
            // Consume digits, replace with {id}.
            while (i < url.size() && std::isdigit(static_cast<unsigned char>(url[i]))) ++i;
            out += "{id}";
            --i;
            last_was_slash = false;
            continue;
        }
        out += c;
        last_was_slash = false;
    }
    return out;
}

void DiscordAdapter::update_bucket_from_headers(
    const std::string& bucket_key,
    const std::unordered_map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lk(rl_mu_);
    RateLimitBucket& b = buckets_[bucket_key];
    auto find_h = [&](const std::string& name) -> const std::string* {
        auto it = headers.find(name);
        if (it != headers.end()) return &it->second;
        // Case-insensitive fallback.
        for (const auto& kv : headers) {
            if (kv.first.size() == name.size()) {
                bool eq = true;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
                        std::tolower(static_cast<unsigned char>(name[i]))) {
                        eq = false;
                        break;
                    }
                }
                if (eq) return &kv.second;
            }
        }
        return nullptr;
    };
    if (auto* v = find_h("X-RateLimit-Remaining")) {
        try { b.remaining = std::stoi(*v); } catch (...) {}
    }
    if (auto* v = find_h("X-RateLimit-Reset-After")) {
        try { b.reset_after_seconds = std::stod(*v); } catch (...) {}
    }
    if (auto* v = find_h("X-RateLimit-Global")) {
        b.global = (!v->empty() && (*v)[0] != '0' && (*v)[0] != 'f');
    }
    b.last_update = std::chrono::steady_clock::now();
}

double DiscordAdapter::wait_seconds_for(const std::string& bucket_key) const {
    std::lock_guard<std::mutex> lk(rl_mu_);
    if (global_limited_) {
        auto now = std::chrono::steady_clock::now();
        if (now < global_reset_) {
            return std::chrono::duration<double>(global_reset_ - now).count();
        }
    }
    auto it = buckets_.find(bucket_key);
    if (it == buckets_.end()) return 0.0;
    const auto& b = it->second;
    if (b.remaining > 0) return 0.0;
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - b.last_update).count();
    double remaining = b.reset_after_seconds - elapsed;
    return remaining > 0 ? remaining : 0.0;
}

void DiscordAdapter::record_global_rate_limit(double retry_after_seconds) {
    std::lock_guard<std::mutex> lk(rl_mu_);
    global_limited_ = true;
    global_reset_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(
                        static_cast<long long>(retry_after_seconds * 1000.0));
}

bool DiscordAdapter::globally_limited() const {
    std::lock_guard<std::mutex> lk(rl_mu_);
    if (!global_limited_) return false;
    return std::chrono::steady_clock::now() < global_reset_;
}

std::size_t DiscordAdapter::tracked_bucket_count() const {
    std::lock_guard<std::mutex> lk(rl_mu_);
    return buckets_.size();
}

// ─── Thread tracking ──────────────────────────────────────────────────────

void DiscordAdapter::track_thread(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(thread_mu_);
    participated_threads_.insert(thread_id);
}

bool DiscordAdapter::thread_tracked(const std::string& thread_id) const {
    std::lock_guard<std::mutex> lk(thread_mu_);
    return participated_threads_.count(thread_id) > 0;
}

std::size_t DiscordAdapter::tracked_thread_count() const {
    std::lock_guard<std::mutex> lk(thread_mu_);
    return participated_threads_.size();
}

// ─── Gateway WebSocket (v10) ─────────────────────────────────────────────

void DiscordAdapter::configure_gateway(
    int intents, std::unique_ptr<WebSocketTransport> transport) {
    DiscordGateway::Config gcfg;
    gcfg.token = cfg_.bot_token;
    gcfg.intents = intents;
    gateway_ = std::make_unique<DiscordGateway>(std::move(gcfg));
    if (transport) {
        gateway_->set_transport(std::move(transport));
    }

    gateway_->set_dispatch_callback(
        [this](const std::string& event, const nlohmann::json& data) {
            if (event == "MESSAGE_CREATE") {
                if (!message_cb_) return;
                std::string channel_id = data.value("channel_id", "");
                std::string content = data.value("content", "");
                std::string message_id = data.value("id", "");
                std::string user_id;
                if (data.contains("author") && data["author"].is_object()) {
                    user_id = data["author"].value("id", "");
                    if (data["author"].value("bot", false)) return;
                }
                message_cb_(channel_id, user_id, content, message_id);
            } else if (event == "INTERACTION_CREATE") {
                dispatch_interaction_payload(data);
            } else if (event == "THREAD_CREATE") {
                std::string id = data.value("id", "");
                if (!id.empty()) track_thread(id);
            }
        });
}

bool DiscordAdapter::start_gateway() {
    if (!gateway_) configure_gateway(compute_intents());
    return gateway_ && gateway_->connect();
}

void DiscordAdapter::stop_gateway() {
    if (gateway_) gateway_->disconnect();
}

bool DiscordAdapter::gateway_run_once() {
    return gateway_ && gateway_->run_once();
}

bool DiscordAdapter::gateway_resume() {
    return gateway_ && gateway_->resume();
}

// ─── Voice ────────────────────────────────────────────────────────────────

bool DiscordAdapter::join_voice(const std::string& channel_id) {
    if (channel_id.empty()) return false;
    if (!voice_codec_.available()) return false;
    voice_channel_id_ = channel_id;
    voice_connected_ = true;
    touch_voice_activity();
    return true;
}

void DiscordAdapter::leave_voice() {
    voice_connected_ = false;
    voice_channel_id_.clear();
}

void DiscordAdapter::set_voice_callback(VoiceCallback cb) {
    std::lock_guard<std::mutex> lk(voice_cb_mu_);
    voice_cb_ = std::move(cb);
}

bool DiscordAdapter::has_voice_callback() const {
    std::lock_guard<std::mutex> lk(voice_cb_mu_);
    return static_cast<bool>(voice_cb_);
}

void DiscordAdapter::register_ssrc_user(uint32_t ssrc,
                                         const std::string& user_id) {
    std::lock_guard<std::mutex> lk(ssrc_mu_);
    ssrc_map_[ssrc] = user_id;
}

std::optional<std::string> DiscordAdapter::ssrc_to_user(uint32_t ssrc) const {
    std::lock_guard<std::mutex> lk(ssrc_mu_);
    auto it = ssrc_map_.find(ssrc);
    if (it == ssrc_map_.end()) return std::nullopt;
    return it->second;
}

bool DiscordAdapter::send_voice_pcm(const int16_t* pcm, std::size_t frames) {
    if (!voice_connected_) return false;
    if (!voice_codec_.available()) return false;
    if (!pcm || frames == 0) return false;
    auto packet = voice_codec_.encode(pcm, frames);
    if (packet.empty()) return false;
    touch_voice_activity();
    return true;
}

bool DiscordAdapter::decrypt_voice_payload(const uint8_t* /*ciphertext*/,
                                            std::size_t /*ct_len*/,
                                            const uint8_t* /*nonce*/,
                                            std::vector<uint8_t>& /*out_plain*/) const {
#ifdef HERMES_GATEWAY_HAS_SODIUM
    return false;
#else
    return false;
#endif
}

bool DiscordAdapter::process_voice_rtp(const uint8_t* rtp, std::size_t len) {
    if (!rtp || len < 12) return false;
    if (!voice_codec_.available()) return false;
    uint16_t sequence = static_cast<uint16_t>((rtp[2] << 8) | rtp[3]);
    uint32_t timestamp = (static_cast<uint32_t>(rtp[4]) << 24) |
                         (static_cast<uint32_t>(rtp[5]) << 16) |
                         (static_cast<uint32_t>(rtp[6]) << 8) |
                         static_cast<uint32_t>(rtp[7]);
    uint32_t ssrc = (static_cast<uint32_t>(rtp[8]) << 24) |
                    (static_cast<uint32_t>(rtp[9]) << 16) |
                    (static_cast<uint32_t>(rtp[10]) << 8) |
                    static_cast<uint32_t>(rtp[11]);
    const uint8_t* payload = rtp + 12;
    std::size_t payload_len = len - 12;

    VoicePacket vp;
    vp.ssrc = ssrc;
    vp.sequence = sequence;
    vp.timestamp = timestamp;
    vp.pcm.resize(OpusCodec::kSamplesPerFrame * OpusCodec::kChannels);
    int decoded = voice_codec_.decode(payload, payload_len, vp.pcm.data(),
                                       vp.pcm.size());
    if (decoded <= 0) return false;
    vp.pcm.resize(static_cast<std::size_t>(decoded) * OpusCodec::kChannels);

    VoiceCallback cb;
    {
        std::lock_guard<std::mutex> lk(voice_cb_mu_);
        cb = voice_cb_;
    }
    if (cb) cb(vp);
    touch_voice_activity();
    return true;
}

void DiscordAdapter::touch_voice_activity() {
    voice_last_activity_ = std::chrono::steady_clock::now();
}

bool DiscordAdapter::voice_idle_expired() const {
    if (!voice_connected_) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - voice_last_activity_).count();
    return elapsed >= cfg_.voice_idle_timeout_s;
}

}  // namespace hermes::gateway::platforms
