// C++17 port of the pure-logic helpers from `hermes_cli/webhook.py`.

#include "hermes/cli/webhook_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace hermes::cli::webhook_cmd {
namespace {

std::string trim(const std::string& value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string{first, last};
}

std::string to_lower(const std::string& value) {
    std::string out{value};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string replace_spaces_with_hyphens(const std::string& value) {
    std::string out{value};
    std::replace(out.begin(), out.end(), ' ', '-');
    return out;
}

}  // namespace

std::string normalize_subscription_name(const std::string& raw) {
    return replace_spaces_with_hyphens(to_lower(trim(raw)));
}

bool is_valid_subscription_name(const std::string& name) {
    static const std::regex pattern{"^[a-z0-9][a-z0-9_-]*$"};
    return std::regex_match(name, pattern);
}

std::vector<std::string> parse_csv_field(const std::string& csv) {
    std::vector<std::string> out{};
    if (csv.empty()) {
        return out;
    }
    std::string::size_type start{0};
    while (start <= csv.size()) {
        auto comma = csv.find(',', start);
        std::string token{};
        if (comma == std::string::npos) {
            token = csv.substr(start);
            start = csv.size() + 1;
        } else {
            token = csv.substr(start, comma - start);
            start = comma + 1;
        }
        out.push_back(trim(token));
    }
    return out;
}

std::string build_webhook_base_url(const std::string& host, int port) {
    std::string display_host{host == "0.0.0.0" ? std::string{"localhost"} : host};
    std::ostringstream oss{};
    oss << "http://" << display_host << ":" << port;
    return oss.str();
}

std::string build_subscription_url(const std::string& base_url,
                                   const std::string& name) {
    return base_url + std::string{"/webhooks/"} + name;
}

nlohmann::json build_subscription_record(const subscribe_inputs& inputs) {
    nlohmann::json route = nlohmann::json::object();
    route["description"] = inputs.description.empty()
                               ? std::string{"Agent-created subscription: "} +
                                     inputs.name
                               : inputs.description;
    route["events"] = parse_csv_field(inputs.events_csv);
    route["secret"] = inputs.secret;
    route["prompt"] = inputs.prompt;
    route["skills"] = parse_csv_field(inputs.skills_csv);
    route["deliver"] = inputs.deliver.empty() ? std::string{"log"}
                                              : inputs.deliver;
    route["created_at"] = inputs.created_at_iso;

    if (!inputs.deliver_chat_id.empty()) {
        nlohmann::json deliver_extra = nlohmann::json::object();
        deliver_extra["chat_id"] = inputs.deliver_chat_id;
        route["deliver_extra"] = deliver_extra;
    }

    return route;
}

std::string format_signature_header(const std::string& hmac_hex) {
    return std::string{"sha256="} + hmac_hex;
}

std::string truncate_prompt_preview(const std::string& prompt) {
    if (prompt.size() <= 80) {
        return prompt;
    }
    return prompt.substr(0, 80) + std::string{"..."};
}

std::vector<std::string> format_list_entry(const std::string& name,
                                           const nlohmann::json& route,
                                           const std::string& base_url) {
    std::vector<std::string> lines{};
    lines.push_back(std::string{"  * "} + name);

    std::string description{};
    if (route.is_object()) {
        auto it = route.find("description");
        if (it != route.end() && it->is_string()) {
            description = it->get<std::string>();
        }
    }
    if (!description.empty()) {
        lines.push_back(std::string{"    "} + description);
    }

    lines.push_back(std::string{"    URL:     "} +
                    build_subscription_url(base_url, name));

    std::string events_csv{};
    if (route.is_object()) {
        auto it = route.find("events");
        if (it != route.end() && it->is_array()) {
            bool first{true};
            for (const auto& ev : *it) {
                if (!ev.is_string()) {
                    continue;
                }
                if (!first) {
                    events_csv += ", ";
                }
                first = false;
                events_csv += ev.get<std::string>();
            }
        }
    }
    lines.push_back(std::string{"    Events:  "} +
                    (events_csv.empty() ? std::string{"(all)"} : events_csv));

    std::string deliver{"log"};
    if (route.is_object()) {
        auto it = route.find("deliver");
        if (it != route.end() && it->is_string()) {
            std::string value{it->get<std::string>()};
            if (!value.empty()) {
                deliver = value;
            }
        }
    }
    lines.push_back(std::string{"    Deliver: "} + deliver);
    lines.emplace_back("");
    return lines;
}

std::vector<std::string> format_subscribe_summary(
    const std::string& name,
    const nlohmann::json& route,
    const std::string& base_url,
    bool is_update) {
    std::vector<std::string> lines{};
    std::string status{is_update ? "Updated" : "Created"};
    lines.emplace_back("");
    lines.push_back(std::string{"  "} + status +
                    std::string{" webhook subscription: "} + name);
    lines.push_back(std::string{"  URL:    "} +
                    build_subscription_url(base_url, name));

    std::string secret{};
    std::string prompt{};
    std::string deliver{"log"};
    std::vector<std::string> events{};

    if (route.is_object()) {
        auto secret_it = route.find("secret");
        if (secret_it != route.end() && secret_it->is_string()) {
            secret = secret_it->get<std::string>();
        }
        auto prompt_it = route.find("prompt");
        if (prompt_it != route.end() && prompt_it->is_string()) {
            prompt = prompt_it->get<std::string>();
        }
        auto deliver_it = route.find("deliver");
        if (deliver_it != route.end() && deliver_it->is_string()) {
            std::string v{deliver_it->get<std::string>()};
            if (!v.empty()) {
                deliver = v;
            }
        }
        auto events_it = route.find("events");
        if (events_it != route.end() && events_it->is_array()) {
            for (const auto& ev : *events_it) {
                if (ev.is_string()) {
                    events.push_back(ev.get<std::string>());
                }
            }
        }
    }

    lines.push_back(std::string{"  Secret: "} + secret);
    if (!events.empty()) {
        std::string joined{};
        for (std::size_t i{0}; i < events.size(); ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += events[i];
        }
        lines.push_back(std::string{"  Events: "} + joined);
    } else {
        lines.emplace_back("  Events: (all)");
    }
    lines.push_back(std::string{"  Deliver: "} + deliver);
    if (!prompt.empty()) {
        lines.push_back(std::string{"  Prompt: "} +
                        truncate_prompt_preview(prompt));
    }
    lines.emplace_back("");
    lines.emplace_back("  Configure your service to POST to the URL above.");
    lines.emplace_back("  Use the secret for HMAC-SHA256 signature validation.");
    lines.emplace_back(
        "  The gateway must be running to receive events (hermes gateway run).");
    lines.emplace_back("");
    return lines;
}

std::string webhook_setup_hint(const std::string& hermes_home_display) {
    std::ostringstream oss{};
    oss << "\n"
        << "  Webhook platform is not enabled. To set it up:\n"
        << "\n"
        << "  1. Run the gateway setup wizard:\n"
        << "     hermes gateway setup\n"
        << "\n"
        << "  2. Or manually add to " << hermes_home_display
        << "/config.yaml:\n"
        << "     platforms:\n"
        << "       webhook:\n"
        << "         enabled: true\n"
        << "         extra:\n"
        << "           host: \"0.0.0.0\"\n"
        << "           port: 8644\n"
        << "           secret: \"your-global-hmac-secret\"\n"
        << "\n"
        << "  3. Or set environment variables in " << hermes_home_display
        << "/.env:\n"
        << "     WEBHOOK_ENABLED=true\n"
        << "     WEBHOOK_PORT=8644\n"
        << "     WEBHOOK_SECRET=your-global-secret\n"
        << "\n"
        << "  Then start the gateway: hermes gateway run\n";
    return oss.str();
}

}  // namespace hermes::cli::webhook_cmd
