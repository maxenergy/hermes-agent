#include <hermes/gateway/config_validator.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hermes::gateway {

namespace {

bool is_placeholder(std::string_view s) {
    if (s.empty()) return true;
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    static const char* kPlaceholders[] = {
        "your_token_here", "changeme", "replace-me", "todo",
        "fill-me-in", "xxxx", "yourtokenhere",
    };
    for (auto p : kPlaceholders) {
        if (lower.find(p) != std::string::npos) return true;
    }
    return false;
}

bool has_whitespace(std::string_view s) {
    for (char c : s)
        if (std::isspace(static_cast<unsigned char>(c))) return true;
    return false;
}

void add_diag(ValidationReport& r, ValidationSeverity sev, std::string path,
               std::string msg) {
    if (sev == ValidationSeverity::Error) r.ok = false;
    r.diagnostics.push_back({sev, std::move(path), std::move(msg)});
}

}  // namespace

// --- ValidationReport ----------------------------------------------------

std::size_t ValidationReport::count(ValidationSeverity sev) const {
    std::size_t n = 0;
    for (auto& d : diagnostics)
        if (d.severity == sev) ++n;
    return n;
}

std::vector<ValidationDiagnostic> ValidationReport::filter(
    ValidationSeverity sev) const {
    std::vector<ValidationDiagnostic> out;
    for (auto& d : diagnostics)
        if (d.severity == sev) out.push_back(d);
    return out;
}

std::string ValidationReport::to_string() const {
    std::ostringstream os;
    os << "ValidationReport(ok=" << (ok ? "true" : "false") << ", errors="
       << count(ValidationSeverity::Error)
       << ", warnings=" << count(ValidationSeverity::Warning)
       << ", info=" << count(ValidationSeverity::Info) << ")";
    for (auto& d : diagnostics) {
        os << "\n  ";
        switch (d.severity) {
            case ValidationSeverity::Error: os << "[ERROR] "; break;
            case ValidationSeverity::Warning: os << "[WARN]  "; break;
            case ValidationSeverity::Info: os << "[INFO]  "; break;
        }
        os << d.path << ": " << d.message;
    }
    return os.str();
}

// --- platform schemas ----------------------------------------------------

const std::vector<PlatformSchema>& platform_schemas() {
    static const std::vector<PlatformSchema> schemas = {
        {Platform::Telegram,
          {"token"},
          {"home_channel", "reply_to_mode"},
          {"token"}},
        {Platform::Discord,
          {"token"},
          {"intents", "home_channel"},
          {"token"}},
        {Platform::Slack,
          {"bot_token"},
          {"app_token", "signing_secret", "home_channel"},
          {"bot_token", "app_token", "signing_secret"}},
        {Platform::WhatsApp,
          {"phone_number"},
          {"credentials_path"},
          {}},
        {Platform::Signal,
          {"username"},
          {"group_id"},
          {}},
        {Platform::Matrix,
          {"homeserver", "access_token"},
          {"room_id", "device_id"},
          {"access_token"}},
        {Platform::Email,
          {"imap_host", "username"},
          {"imap_port", "password", "smtp_host", "smtp_port"},
          {"password"}},
        {Platform::Sms,
          {"account_sid", "auth_token", "from_number"},
          {},
          {"auth_token"}},
        {Platform::DingTalk,
          {"app_key", "app_secret"},
          {"webhook_url"},
          {"app_secret"}},
        {Platform::Feishu,
          {"app_id", "app_secret"},
          {"verification_token", "encrypt_key"},
          {"app_secret", "encrypt_key"}},
        {Platform::WeCom,
          {"corp_id", "agent_id", "secret"},
          {},
          {"secret"}},
        {Platform::Weixin,
          {"app_id", "app_secret"},
          {"token"},
          {"app_secret", "token"}},
        {Platform::BlueBubbles,
          {"server_url", "password"},
          {},
          {"password"}},
        {Platform::HomeAssistant,
          {"base_url", "access_token"},
          {},
          {"access_token"}},
        {Platform::ApiServer,
          {},
          {"listen_port", "listen_host", "api_token"},
          {"api_token"}},
        {Platform::Webhook,
          {"url"},
          {"secret", "headers"},
          {"secret"}},
    };
    return schemas;
}

const PlatformSchema* schema_for(Platform platform) {
    for (auto& s : platform_schemas()) {
        if (s.platform == platform) return &s;
    }
    return nullptr;
}

// --- is_plausible_secret ------------------------------------------------

bool is_plausible_secret(std::string_view value) {
    if (value.size() < 8) return false;
    if (has_whitespace(value)) return false;
    if (is_placeholder(value)) return false;
    return true;
}

// --- merge_gateway_configs ----------------------------------------------

nlohmann::json merge_gateway_configs(const nlohmann::json& base,
                                      const nlohmann::json& overrides) {
    if (!base.is_object() || !overrides.is_object()) return overrides;
    nlohmann::json out = base;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (out.contains(it.key()) && out[it.key()].is_object() &&
            it.value().is_object()) {
            out[it.key()] = merge_gateway_configs(out[it.key()], it.value());
        } else {
            out[it.key()] = it.value();
        }
    }
    return out;
}

// --- redact_secrets ------------------------------------------------------

nlohmann::json redact_secrets(const nlohmann::json& config) {
    nlohmann::json out = config;
    if (!out.is_object()) return out;
    if (!out.contains("platforms") || !out["platforms"].is_object())
        return out;

    auto& platforms = out["platforms"];
    for (auto it = platforms.begin(); it != platforms.end(); ++it) {
        Platform p;
        try {
            p = platform_from_string(it.key());
        } catch (...) {
            continue;
        }
        const auto* schema = schema_for(p);
        if (!schema) continue;
        for (auto& k : schema->secret_fields) {
            if (it.value().contains(k)) {
                it.value()[k] = "***REDACTED***";
            }
        }
    }
    return out;
}

// --- inject_defaults -----------------------------------------------------

namespace {

void ensure_key(nlohmann::json& obj, const char* key,
                 const nlohmann::json& def, std::size_t& counter) {
    if (!obj.contains(key)) {
        obj[key] = def;
        ++counter;
    }
}

}  // namespace

std::size_t inject_defaults(nlohmann::json& config) {
    std::size_t injected = 0;
    if (!config.is_object()) {
        config = nlohmann::json::object();
        ++injected;
    }

    ensure_key(config, "config_version", kCurrentConfigVersion, injected);
    ensure_key(config, "sessions_dir", "~/.hermes/gateway/sessions", injected);
    ensure_key(config, "group_sessions_per_user", false, injected);
    ensure_key(config, "thread_sessions_per_user", false, injected);
    ensure_key(config, "unauthorized_dm_behavior", "pair", injected);

    if (!config.contains("reset_policy") ||
        !config["reset_policy"].is_object()) {
        config["reset_policy"] = nlohmann::json::object();
        ++injected;
    }
    auto& rp = config["reset_policy"];
    ensure_key(rp, "mode", "daily", injected);
    ensure_key(rp, "at_hour", 0, injected);
    ensure_key(rp, "idle_minutes", 1440, injected);
    ensure_key(rp, "notify", true, injected);

    if (!config.contains("platforms") || !config["platforms"].is_object()) {
        config["platforms"] = nlohmann::json::object();
        ++injected;
    }
    auto& platforms = config["platforms"];
    for (auto& schema : platform_schemas()) {
        auto key = platform_to_string(schema.platform);
        if (!platforms.contains(key)) continue;  // don't add disabled blocks
        auto& block = platforms[key];
        if (!block.is_object()) {
            block = nlohmann::json::object();
            ++injected;
        }
        ensure_key(block, "enabled", false, injected);
        ensure_key(block, "reply_to_mode", "off", injected);
    }

    // Backpressure defaults.
    if (!config.contains("backpressure")) {
        config["backpressure"] = {{"max_per_session", 16},
                                    {"max_total", 1024},
                                    {"policy", "drop-oldest"},
                                    {"coalesce", true},
                                    {"max_age_seconds", 300}};
        injected += 5;
    }

    // Metrics defaults.
    if (!config.contains("metrics")) {
        config["metrics"] = {{"enabled", true}, {"interval_ms", 30000}};
        injected += 2;
    }

    return injected;
}

// --- migrate_gateway_config ---------------------------------------------

int migrate_gateway_config(nlohmann::json& config) {
    if (!config.is_object()) {
        config = nlohmann::json::object();
    }
    int from = -1;
    if (config.contains("config_version") &&
        config["config_version"].is_number_integer()) {
        from = config["config_version"].get<int>();
    } else {
        from = 1;  // assume unversioned => v1
    }
    int v = from;

    // v1 -> v2: sessions_dir becomes required.
    if (v < 2) {
        if (!config.contains("sessions_dir")) {
            config["sessions_dir"] = "~/.hermes/gateway/sessions";
        }
        v = 2;
    }
    // v2 -> v3: add reset_policy.
    if (v < 3) {
        if (!config.contains("reset_policy")) {
            config["reset_policy"] = {{"mode", "daily"}, {"at_hour", 0}};
        }
        v = 3;
    }
    // v3 -> v4: rename legacy ``per_user_sessions`` flag.
    if (v < 4) {
        if (config.contains("per_user_sessions")) {
            config["group_sessions_per_user"] = config["per_user_sessions"];
            config.erase("per_user_sessions");
        }
        v = 4;
    }
    // v4 -> v5: split thread flag.
    if (v < 5) {
        if (!config.contains("thread_sessions_per_user")) {
            config["thread_sessions_per_user"] = false;
        }
        v = 5;
    }
    // v5 -> v6: add unauthorized_dm_behavior.
    if (v < 6) {
        if (!config.contains("unauthorized_dm_behavior")) {
            config["unauthorized_dm_behavior"] = "pair";
        }
        v = 6;
    }
    // v6 -> v7: add backpressure and metrics blocks.
    if (v < 7) {
        if (!config.contains("backpressure")) {
            config["backpressure"] = {{"max_per_session", 16},
                                        {"max_total", 1024},
                                        {"policy", "drop-oldest"}};
        }
        if (!config.contains("metrics")) {
            config["metrics"] = {{"enabled", true},
                                  {"interval_ms", 30000}};
        }
        v = 7;
    }

    config["config_version"] = kCurrentConfigVersion;
    return from;
}

// --- validate_gateway_config --------------------------------------------

ValidationReport validate_gateway_config(const nlohmann::json& config) {
    ValidationReport r;
    if (!config.is_object()) {
        add_diag(r, ValidationSeverity::Error, "",
                  "gateway config must be a JSON object");
        return r;
    }

    // config_version.
    if (!config.contains("config_version")) {
        add_diag(r, ValidationSeverity::Warning, "config_version",
                  "missing — defaulting to current");
    } else if (!config["config_version"].is_number_integer()) {
        add_diag(r, ValidationSeverity::Error, "config_version",
                  "must be integer");
    } else {
        int v = config["config_version"].get<int>();
        if (v > kCurrentConfigVersion) {
            add_diag(r, ValidationSeverity::Warning, "config_version",
                      "newer than supported (" +
                          std::to_string(kCurrentConfigVersion) + ")");
        }
    }

    // sessions_dir.
    if (!config.contains("sessions_dir")) {
        add_diag(r, ValidationSeverity::Warning, "sessions_dir",
                  "missing — defaulting to ~/.hermes/gateway/sessions");
    } else if (!config["sessions_dir"].is_string() ||
               config["sessions_dir"].get<std::string>().empty()) {
        add_diag(r, ValidationSeverity::Error, "sessions_dir",
                  "must be a non-empty string");
    }

    // reset_policy.
    if (config.contains("reset_policy")) {
        auto& rp = config["reset_policy"];
        if (!rp.is_object()) {
            add_diag(r, ValidationSeverity::Error, "reset_policy",
                      "must be object");
        } else {
            if (rp.contains("mode") && rp["mode"].is_string()) {
                auto mode = rp["mode"].get<std::string>();
                if (mode != "daily" && mode != "idle" && mode != "both" &&
                    mode != "none") {
                    add_diag(r, ValidationSeverity::Error,
                              "reset_policy.mode",
                              "must be one of daily|idle|both|none");
                }
            }
            if (rp.contains("at_hour") && rp["at_hour"].is_number_integer()) {
                int h = rp["at_hour"].get<int>();
                if (h < 0 || h > 23) {
                    add_diag(r, ValidationSeverity::Error,
                              "reset_policy.at_hour",
                              "must be 0..23");
                }
            }
            if (rp.contains("idle_minutes") &&
                rp["idle_minutes"].is_number_integer()) {
                int m = rp["idle_minutes"].get<int>();
                if (m <= 0) {
                    add_diag(r, ValidationSeverity::Error,
                              "reset_policy.idle_minutes",
                              "must be positive");
                }
            }
        }
    }

    // platforms block.
    if (!config.contains("platforms")) {
        add_diag(r, ValidationSeverity::Warning, "platforms",
                  "no platforms configured — gateway will have nothing to do");
    } else if (!config["platforms"].is_object()) {
        add_diag(r, ValidationSeverity::Error, "platforms",
                  "must be object");
    } else {
        auto& platforms = config["platforms"];
        bool any_enabled = false;
        for (auto it = platforms.begin(); it != platforms.end(); ++it) {
            auto name = it.key();
            auto& block = it.value();
            std::string base = "platforms." + name;
            if (!block.is_object()) {
                add_diag(r, ValidationSeverity::Error, base,
                          "must be object");
                continue;
            }

            Platform p;
            try {
                p = platform_from_string(name);
            } catch (...) {
                add_diag(r, ValidationSeverity::Warning, base,
                          "unknown platform identifier");
                continue;
            }

            bool enabled = block.value("enabled", false);
            if (enabled) any_enabled = true;

            const auto* schema = schema_for(p);
            if (!schema) continue;

            if (enabled) {
                for (auto& req : schema->required_fields) {
                    if (!block.contains(req) ||
                        (block[req].is_string() &&
                         block[req].get<std::string>().empty())) {
                        add_diag(r, ValidationSeverity::Error,
                                  base + "." + req,
                                  "required when platform is enabled");
                    }
                }
                for (auto& sec : schema->secret_fields) {
                    if (block.contains(sec) && block[sec].is_string()) {
                        auto val = block[sec].get<std::string>();
                        if (!val.empty() && !is_plausible_secret(val)) {
                            add_diag(r, ValidationSeverity::Warning,
                                      base + "." + sec,
                                      "looks like a placeholder");
                        }
                    }
                }
            }

            // reply_to_mode sanity.
            if (block.contains("reply_to_mode") &&
                block["reply_to_mode"].is_string()) {
                auto m = block["reply_to_mode"].get<std::string>();
                if (m != "off" && m != "first" && m != "all") {
                    add_diag(r, ValidationSeverity::Error,
                              base + ".reply_to_mode",
                              "must be one of off|first|all");
                }
            }
        }
        if (!any_enabled) {
            add_diag(r, ValidationSeverity::Info, "platforms",
                      "no platforms enabled");
        }
    }

    // backpressure bounds.
    if (config.contains("backpressure") && config["backpressure"].is_object()) {
        auto& bp = config["backpressure"];
        auto per = bp.value("max_per_session", 16);
        auto tot = bp.value("max_total", 1024);
        if (per <= 0) {
            add_diag(r, ValidationSeverity::Error,
                      "backpressure.max_per_session",
                      "must be positive");
        }
        if (tot <= 0 || tot < per) {
            add_diag(r, ValidationSeverity::Error, "backpressure.max_total",
                      "must be >= max_per_session");
        }
        if (bp.contains("policy") && bp["policy"].is_string()) {
            auto p = bp["policy"].get<std::string>();
            if (p != "drop-oldest" && p != "drop-newest" && p != "reject") {
                add_diag(r, ValidationSeverity::Error,
                          "backpressure.policy",
                          "must be one of drop-oldest|drop-newest|reject");
            }
        }
    }

    // metrics bounds.
    if (config.contains("metrics") && config["metrics"].is_object()) {
        auto& m = config["metrics"];
        if (m.contains("interval_ms") && m["interval_ms"].is_number_integer()) {
            int iv = m["interval_ms"].get<int>();
            if (iv < 1000) {
                add_diag(r, ValidationSeverity::Warning,
                          "metrics.interval_ms",
                          "< 1s intervals may hurt performance");
            }
        }
    }

    return r;
}

}  // namespace hermes::gateway
