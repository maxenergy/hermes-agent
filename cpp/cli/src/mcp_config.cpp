// C++17 port of hermes_cli/mcp_config.py.  See header for scope notes.
#include "hermes/cli/mcp_config.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#endif

namespace hermes::cli::mcp_config {

namespace {

namespace col = hermes::cli::colors;

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void info_(const std::string& t) {
    std::cout << "  " << col::dim(t) << "\n";
}
void success_(const std::string& t) {
    std::cout << "  " << col::green("\xe2\x9c\x93 " + t) << "\n";
}
void warning_(const std::string& t) {
    std::cout << "  " << col::yellow("\xe2\x9a\xa0 " + t) << "\n";
}
void error_(const std::string& t) {
    std::cout << "  " << col::red("\xe2\x9c\x97 " + t) << "\n";
}

bool confirm_(const std::string& question, bool default_yes) {
    std::cout << "  " << col::yellow(question)
              << (default_yes ? " [Y/n]: " : " [y/N]: ");
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cout << "\n";
        return default_yes;
    }
    line = to_lower(trim(line));
    if (line.empty()) return default_yes;
    return line == "y" || line == "yes";
}

[[maybe_unused]] std::string prompt_(const std::string& question,
                                     const std::string& default_val = "") {
    std::cout << "  " << col::yellow(question);
    if (!default_val.empty()) {
        std::cout << " [" << default_val << "]";
    }
    std::cout << ": ";
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cout << "\n";
        return default_val;
    }
    line = trim(line);
    return line.empty() ? default_val : line;
}

// ---- URL parsing helper (host + port only) ----------------------
struct ParsedURL {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path = "/";
};

ParsedURL parse_url(const std::string& url) {
    ParsedURL out;
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return out;
    out.scheme = to_lower(url.substr(0, schemeEnd));
    auto rest = url.substr(schemeEnd + 3);
    auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest
                                                         : rest.substr(0, slash);
    if (slash != std::string::npos) out.path = rest.substr(slash);
    auto atSign = authority.find('@');
    if (atSign != std::string::npos) {
        authority = authority.substr(atSign + 1);
    }
    auto colon = authority.find(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            out.port = 0;
        }
    } else {
        out.host = authority;
        if (out.scheme == "https") {
            out.port = 443;
        } else if (out.scheme == "http") {
            out.port = 80;
        }
    }
    return out;
}

bool tcp_reachable(const std::string& host, int port, int timeout_ms = 3000) {
#ifdef _WIN32
    (void)host; (void)port; (void)timeout_ms;
    return false;
#else
    if (host.empty() || port <= 0) return false;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }
    int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ::freeaddrinfo(res);
        return false;
    }
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool ok = (::connect(sock, res->ai_addr, res->ai_addrlen) == 0);
    ::close(sock);
    ::freeaddrinfo(res);
    return ok;
#endif
}

bool command_on_path(const std::string& command) {
    if (command.empty()) return false;
    if (command.find('/') != std::string::npos) {
        // Explicit path — just check existence.
        return std::filesystem::exists(command);
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::stringstream ss(path_env);
    std::string dir;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) continue;
        std::filesystem::path p = std::filesystem::path(dir) / command;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return true;
    }
    return false;
}

}  // namespace

// ---- Public helpers -------------------------------------------

std::string env_key_for_server(const std::string& server_name) {
    std::string upper = to_upper(server_name);
    std::replace(upper.begin(), upper.end(), '-', '_');
    std::replace(upper.begin(), upper.end(), '.', '_');
    return "MCP_" + upper + "_API_KEY";
}

std::string interpolate_env(const std::string& value) {
    static const std::regex re(R"(\$\{(\w+)\})");
    std::string result;
    auto begin = std::sregex_iterator(value.begin(), value.end(), re);
    auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        result.append(value, last, m.position() - last);
        const char* env_val = std::getenv(m[1].str().c_str());
        if (env_val) result.append(env_val);
        last = m.position() + m.length();
    }
    result.append(value, last, std::string::npos);
    return result;
}

std::string mask_auth_value(const std::string& value) {
    std::string resolved = interpolate_env(value);
    if (resolved.size() > 8) {
        return resolved.substr(0, 4) + "***" +
               resolved.substr(resolved.size() - 4);
    }
    return "***";
}

// ---- ServerConfig <-> JSON round-trip --------------------------

nlohmann::json ServerConfig::to_json() const {
    nlohmann::json j = extra;
    if (!url.empty()) {
        j["url"] = url;
    }
    if (!command.empty()) {
        j["command"] = command;
        if (!args.empty()) j["args"] = args;
    }
    if (!auth_type.empty()) {
        j["auth"] = auth_type;
    }
    if (!headers.empty()) {
        nlohmann::json h = nlohmann::json::object();
        for (const auto& [k, v] : headers) h[k] = v;
        j["headers"] = h;
    }
    if (!include.empty() || !exclude.empty()) {
        nlohmann::json t = nlohmann::json::object();
        if (!include.empty()) t["include"] = include;
        if (!exclude.empty()) t["exclude"] = exclude;
        j["tools"] = t;
    }
    j["enabled"] = enabled;
    return j;
}

ServerConfig ServerConfig::from_json(const std::string& name,
                                     const nlohmann::json& j) {
    ServerConfig c;
    c.name = name;
    if (!j.is_object()) return c;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const auto& k = it.key();
        if (k == "url" && it->is_string()) {
            c.url = it->get<std::string>();
        } else if (k == "command" && it->is_string()) {
            c.command = it->get<std::string>();
        } else if (k == "args" && it->is_array()) {
            for (const auto& a : *it) {
                c.args.push_back(a.is_string() ? a.get<std::string>()
                                               : a.dump());
            }
        } else if (k == "auth" && it->is_string()) {
            c.auth_type = it->get<std::string>();
        } else if (k == "headers" && it->is_object()) {
            for (auto hit = it->begin(); hit != it->end(); ++hit) {
                if (hit->is_string()) {
                    c.headers[hit.key()] = hit->get<std::string>();
                }
            }
        } else if (k == "tools" && it->is_object()) {
            if (it->contains("include") && (*it)["include"].is_array()) {
                for (const auto& t : (*it)["include"]) {
                    if (t.is_string()) c.include.push_back(t.get<std::string>());
                }
            }
            if (it->contains("exclude") && (*it)["exclude"].is_array()) {
                for (const auto& t : (*it)["exclude"]) {
                    if (t.is_string()) c.exclude.push_back(t.get<std::string>());
                }
            }
        } else if (k == "enabled") {
            if (it->is_boolean()) c.enabled = it->get<bool>();
            else if (it->is_string()) {
                std::string v = to_lower(it->get<std::string>());
                c.enabled = (v == "true" || v == "1" || v == "yes");
            }
        } else {
            // Preserve unknown fields.
            c.extra[k] = *it;
        }
    }
    return c;
}

// ---- Config IO -------------------------------------------------

std::map<std::string, ServerConfig> list_servers() {
    std::map<std::string, ServerConfig> out;
    auto cfg = hermes::config::load_config();
    if (!cfg.contains("mcp_servers")) return out;
    const auto& servers = cfg["mcp_servers"];
    if (!servers.is_object()) return out;
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        out.emplace(it.key(),
                    ServerConfig::from_json(it.key(), *it));
    }
    return out;
}

std::optional<ServerConfig> get_server(const std::string& name) {
    auto all = list_servers();
    auto it = all.find(name);
    if (it == all.end()) return std::nullopt;
    return it->second;
}

bool save_server(const ServerConfig& server) {
    if (server.name.empty()) return false;
    auto cfg = hermes::config::load_config();
    if (!cfg.contains("mcp_servers") || !cfg["mcp_servers"].is_object()) {
        cfg["mcp_servers"] = nlohmann::json::object();
    }
    cfg["mcp_servers"][server.name] = server.to_json();
    try {
        hermes::config::save_config(cfg);
    } catch (const std::exception& e) {
        error_(std::string("Failed to save config: ") + e.what());
        return false;
    }
    return true;
}

bool remove_server(const std::string& name) {
    auto cfg = hermes::config::load_config();
    if (!cfg.contains("mcp_servers") || !cfg["mcp_servers"].is_object()) {
        return false;
    }
    auto& servers = cfg["mcp_servers"];
    if (!servers.contains(name)) return false;
    servers.erase(name);
    if (servers.empty()) {
        cfg.erase("mcp_servers");
    }
    try {
        hermes::config::save_config(cfg);
    } catch (const std::exception& e) {
        error_(std::string("Failed to save config: ") + e.what());
        return false;
    }
    return true;
}

// ---- Render helpers --------------------------------------------

RenderedRow render_row(const ServerConfig& server) {
    RenderedRow r;
    r.name = server.name;
    if (server.is_http()) {
        r.transport = server.url.size() > 28
            ? server.url.substr(0, 25) + "..."
            : server.url;
    } else if (server.is_stdio()) {
        std::string t = server.command;
        for (std::size_t i = 0; i < std::min<std::size_t>(2, server.args.size());
             ++i) {
            t += " " + server.args[i];
        }
        if (t.size() > 28) t = t.substr(0, 25) + "...";
        r.transport = t;
    } else {
        r.transport = "?";
    }
    if (!server.include.empty()) {
        r.tools_str = std::to_string(server.include.size()) + " selected";
    } else if (!server.exclude.empty()) {
        r.tools_str = "-" + std::to_string(server.exclude.size()) + " excluded";
    } else {
        r.tools_str = "all";
    }
    r.status = server.enabled ? "enabled" : "disabled";
    return r;
}

std::vector<std::string> render_table(
    const std::map<std::string, ServerConfig>& servers) {
    std::vector<std::string> rows;
    rows.push_back("  Name             Transport                      Tools        Status");
    rows.push_back("  ---------------- ------------------------------ ------------ ----------");
    for (const auto& [_, srv] : servers) {
        auto r = render_row(srv);
        std::string name_padded = r.name;
        name_padded.resize(std::max<std::size_t>(name_padded.size(), 16), ' ');
        std::string transport_padded = r.transport;
        transport_padded.resize(
            std::max<std::size_t>(transport_padded.size(), 30), ' ');
        std::string tools_padded = r.tools_str;
        tools_padded.resize(std::max<std::size_t>(tools_padded.size(), 12), ' ');
        std::string line = "  " + name_padded.substr(0, 16) + " " +
                           transport_padded.substr(0, 30) + " " +
                           tools_padded.substr(0, 12) + " " + r.status;
        rows.push_back(line);
    }
    return rows;
}

// ---- Probe ------------------------------------------------------

ProbeResult probe_server(const ServerConfig& server) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    ProbeResult out;

    if (server.is_http()) {
        auto url = parse_url(server.url);
        if (url.host.empty() || url.port <= 0) {
            out.ok = false;
            out.message = "Invalid URL: " + server.url;
        } else if (tcp_reachable(url.host, url.port)) {
            out.ok = true;
            out.message = "Reachable: " + url.host + ":" +
                          std::to_string(url.port);
        } else {
            out.ok = false;
            out.message = "Unreachable: " + url.host + ":" +
                          std::to_string(url.port);
        }
    } else if (server.is_stdio()) {
        if (command_on_path(server.command)) {
            out.ok = true;
            out.message = "Command found on PATH: " + server.command;
        } else {
            out.ok = false;
            out.message = "Command not on PATH: " + server.command;
        }
    } else {
        out.ok = false;
        out.message = "No transport configured";
    }
    auto end = clock::now();
    out.elapsed_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             end - start).count());
    return out;
}

// ---- Subcommand handlers ---------------------------------------

int cmd_list(const std::vector<std::string>& /*argv*/) {
    auto servers = list_servers();
    if (servers.empty()) {
        std::cout << "\n  No MCP servers configured.\n\n"
                  << "  Add one with:\n"
                  << "    hermes mcp add <name> --url <endpoint>\n"
                  << "    hermes mcp add <name> --command <cmd>\n\n";
        return 0;
    }
    std::cout << "\n  " << col::cyan("MCP Servers:") << "\n\n";
    for (const auto& line : render_table(servers)) {
        std::cout << line << "\n";
    }
    std::cout << "\n";
    return 0;
}

int cmd_add(const std::vector<std::string>& argv) {
    std::string name;
    std::string url;
    std::string command;
    std::vector<std::string> args;
    std::string auth_type;
    bool overwrite = false;

    // Positional: <name>; flags: --url / --command / --args / --auth
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        if (a == "--url" && i + 1 < argv.size()) {
            url = argv[++i];
        } else if (a == "--command" && i + 1 < argv.size()) {
            command = argv[++i];
        } else if (a == "--args") {
            while (i + 1 < argv.size() && argv[i + 1].rfind("--", 0) != 0) {
                args.push_back(argv[++i]);
            }
        } else if (a == "--auth" && i + 1 < argv.size()) {
            auth_type = argv[++i];
        } else if (a == "--overwrite") {
            overwrite = true;
        } else if (!a.empty() && a[0] != '-' && name.empty()) {
            name = a;
        }
    }

    if (name.empty()) {
        error_("Missing server name. Usage: hermes mcp add <name> --url|--command ...");
        return 1;
    }
    if (url.empty() && command.empty()) {
        error_("Must specify --url <endpoint> or --command <cmd>");
        info_("Examples:");
        info_("  hermes mcp add ink --url \"https://mcp.example.com/mcp\"");
        info_("  hermes mcp add github --command npx --args @mcp/server-github");
        return 1;
    }

    auto existing = get_server(name);
    if (existing && !overwrite) {
        if (!confirm_("Server '" + name + "' already exists. Overwrite?",
                      false)) {
            info_("Cancelled.");
            return 0;
        }
    }

    ServerConfig srv;
    srv.name = name;
    srv.url = url;
    srv.command = command;
    srv.args = args;
    srv.auth_type = auth_type;
    srv.enabled = true;

    // For URL transports, auto-add Bearer header referencing env var.
    if (!url.empty() && auth_type != "oauth") {
        srv.headers["Authorization"] =
            "Bearer ${" + env_key_for_server(name) + "}";
    }

    if (!save_server(srv)) return 1;
    success_("Saved '" + name + "' to config.yaml");

    // Optional probe.
    auto probe = probe_server(srv);
    if (probe.ok) {
        success_(probe.message + " (" + std::to_string(probe.elapsed_ms) + "ms)");
    } else {
        warning_(probe.message);
    }
    return 0;
}

int cmd_remove(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        error_("Usage: hermes mcp remove <name>");
        return 1;
    }
    const auto& name = argv[0];
    auto existing = get_server(name);
    if (!existing) {
        error_("Server '" + name + "' not found in config.");
        return 1;
    }
    if (!confirm_("Remove server '" + name + "'?", true)) {
        info_("Cancelled.");
        return 0;
    }
    if (!remove_server(name)) {
        error_("Failed to remove '" + name + "'.");
        return 1;
    }
    success_("Removed '" + name + "' from config");
    return 0;
}

int cmd_test(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        error_("Usage: hermes mcp test <name>");
        return 1;
    }
    const auto& name = argv[0];
    auto srv_opt = get_server(name);
    if (!srv_opt) {
        error_("Server '" + name + "' not found in config.");
        return 1;
    }
    const auto& srv = *srv_opt;
    std::cout << "\n  " << col::cyan("Testing '" + name + "'...") << "\n";
    if (srv.is_http()) {
        info_("Transport: HTTP \xe2\x86\x92 " + srv.url);
    } else {
        info_("Transport: stdio \xe2\x86\x92 " + srv.command);
    }
    if (!srv.auth_type.empty()) {
        info_("Auth: " + srv.auth_type);
    } else if (!srv.headers.empty()) {
        for (const auto& [k, v] : srv.headers) {
            std::string lk = to_lower(k);
            if (lk.find("auth") != std::string::npos ||
                lk.find("key") != std::string::npos) {
                std::cout << "    " << k << ": " << mask_auth_value(v) << "\n";
            }
        }
    } else {
        info_("Auth: none");
    }
    auto probe = probe_server(srv);
    if (probe.ok) {
        success_(probe.message + " (" + std::to_string(probe.elapsed_ms) +
                 "ms)");
        return 0;
    }
    error_(probe.message + " (" + std::to_string(probe.elapsed_ms) + "ms)");
    return 1;
}

int cmd_enable(const std::vector<std::string>& argv, bool enable) {
    if (argv.empty()) {
        error_(std::string("Usage: hermes mcp ") +
               (enable ? "enable" : "disable") + " <name>");
        return 1;
    }
    const auto& name = argv[0];
    auto srv = get_server(name);
    if (!srv) {
        error_("Server '" + name + "' not found.");
        return 1;
    }
    if (srv->enabled == enable) {
        info_(std::string("Server '") + name + "' already " +
              (enable ? "enabled" : "disabled"));
        return 0;
    }
    srv->enabled = enable;
    if (!save_server(*srv)) return 1;
    success_(std::string(enable ? "Enabled" : "Disabled") + " '" + name + "'");
    return 0;
}

int cmd_configure(const std::vector<std::string>& argv) {
    // Non-interactive fallback of the curses-checklist in Python: we
    // accept `--include a,b,c` / `--exclude x,y` / `--clear` and update
    // the filter without opening a TUI.
    if (argv.empty()) {
        error_("Usage: hermes mcp configure <name> "
               "[--include a,b,c] [--exclude x,y] [--clear]");
        return 1;
    }
    std::string name;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    bool clear = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        if (a == "--include" && i + 1 < argv.size()) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                tok = trim(tok);
                if (!tok.empty()) include.push_back(tok);
            }
        } else if (a == "--exclude" && i + 1 < argv.size()) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                tok = trim(tok);
                if (!tok.empty()) exclude.push_back(tok);
            }
        } else if (a == "--clear") {
            clear = true;
        } else if (!a.empty() && a[0] != '-' && name.empty()) {
            name = a;
        }
    }
    if (name.empty()) {
        error_("Missing server name.");
        return 1;
    }
    auto srv = get_server(name);
    if (!srv) {
        error_("Server '" + name + "' not found.");
        return 1;
    }
    if (clear) {
        srv->include.clear();
        srv->exclude.clear();
    }
    if (!include.empty()) {
        srv->include = include;
        srv->exclude.clear();
    }
    if (!exclude.empty()) {
        srv->exclude = exclude;
        srv->include.clear();
    }
    if (!save_server(*srv)) return 1;
    success_("Updated tool filter for '" + name + "'");
    return 0;
}

// ---- Entry dispatch --------------------------------------------

namespace {

void print_help() {
    std::cout << "hermes mcp — manage MCP server entries in config.yaml\n\n"
              << "Usage:\n"
              << "  hermes mcp list\n"
              << "  hermes mcp add <name> --url <endpoint> [--auth oauth|header]\n"
              << "  hermes mcp add <name> --command <cmd> [--args a b ...]\n"
              << "  hermes mcp remove <name>\n"
              << "  hermes mcp test <name>\n"
              << "  hermes mcp enable <name>\n"
              << "  hermes mcp disable <name>\n"
              << "  hermes mcp configure <name> [--include a,b] [--exclude x] [--clear]\n";
}

}  // namespace

int run(int argc, char* argv[]) {
    if (argc <= 2) {
        return cmd_list({});
    }
    std::string sub = argv[2];
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) rest.emplace_back(argv[i]);

    if (sub == "list" || sub == "ls") return cmd_list(rest);
    if (sub == "add") return cmd_add(rest);
    if (sub == "remove" || sub == "rm") return cmd_remove(rest);
    if (sub == "test") return cmd_test(rest);
    if (sub == "enable") return cmd_enable(rest, true);
    if (sub == "disable") return cmd_enable(rest, false);
    if (sub == "configure" || sub == "config") return cmd_configure(rest);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_help();
        return 0;
    }
    error_("Unknown mcp subcommand: " + sub);
    print_help();
    return 1;
}

}  // namespace hermes::cli::mcp_config
