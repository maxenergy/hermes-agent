#include "hermes/auth/credentials.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"
#include "hermes/core/strings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#else
#include <io.h>
#include <sys/stat.h>
#endif

namespace hermes::auth {

namespace fs = std::filesystem;

namespace {

fs::path env_path() {
    return hermes::core::path::get_hermes_home() / ".env";
}

// Parse the env file into an ordered list of (key, raw_line) pairs
// plus "preserve" lines that aren't key=value (comments, blanks).  We
// emit the file back out in the original order so hand-edited comments
// survive an update.
struct EnvLine {
    enum class Kind { KeyValue, Raw };
    Kind kind;
    std::string key;    // only valid when kind == KeyValue
    std::string value;  // only valid when kind == KeyValue
    std::string raw;    // only valid when kind == Raw
};

std::vector<EnvLine> parse(const fs::path& path) {
    std::vector<EnvLine> out;
    std::ifstream in(path);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = hermes::core::strings::trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            out.push_back({EnvLine::Kind::Raw, "", "", line});
            continue;
        }
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            out.push_back({EnvLine::Kind::Raw, "", "", line});
            continue;
        }
        auto key = std::string(hermes::core::strings::trim(
            std::string_view(trimmed).substr(0, eq)));
        auto value = std::string(hermes::core::strings::trim(
            std::string_view(trimmed).substr(eq + 1)));
        // Strip surrounding quotes for the in-memory view, then re-add
        // them on write if the value would otherwise be ambiguous.
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        out.push_back({EnvLine::Kind::KeyValue, std::move(key),
                       std::move(value), ""});
    }
    return out;
}

std::string quote_if_needed(const std::string& value) {
    // Quote when the value contains whitespace, `#`, or control chars.
    bool needs = value.empty();
    for (char c : value) {
        if (c == ' ' || c == '\t' || c == '#' || c == '"' || c == '\'' ||
            static_cast<unsigned char>(c) < 0x20) {
            needs = true;
            break;
        }
    }
    if (!needs) {
        return value;
    }
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string serialise(const std::vector<EnvLine>& lines) {
    std::ostringstream os;
    for (const auto& line : lines) {
        if (line.kind == EnvLine::Kind::Raw) {
            os << line.raw << '\n';
        } else {
            os << line.key << '=' << quote_if_needed(line.value) << '\n';
        }
    }
    return os.str();
}

void secure_perms([[maybe_unused]] const fs::path& path) {
#ifndef _WIN32
    // 0600 — rw for owner only.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    // Set file to owner-only read/write on Windows.
    _chmod(path.string().c_str(), _S_IREAD | _S_IWRITE);
#endif
}

void write_env_file(const fs::path& path, const std::vector<EnvLine>& lines) {
    const auto body = serialise(lines);
    if (!hermes::core::atomic_io::atomic_write(path, body)) {
        throw std::runtime_error(
            "hermes::auth: atomic_write failed for " + path.string());
    }
    secure_perms(path);
}

}  // namespace

void store_credential(std::string_view key, std::string_view value) {
    if (key.empty()) {
        throw std::runtime_error("hermes::auth::store_credential: empty key");
    }
    const auto path = env_path();
    auto lines = parse(path);
    bool found = false;
    for (auto& line : lines) {
        if (line.kind == EnvLine::Kind::KeyValue && line.key == key) {
            line.value = std::string(value);
            found = true;
            break;
        }
    }
    if (!found) {
        lines.push_back({EnvLine::Kind::KeyValue, std::string(key),
                         std::string(value), ""});
    }
    write_env_file(path, lines);
}

std::optional<std::string> get_credential(std::string_view key) {
    const std::string key_str(key);
    const auto path = env_path();
    auto lines = parse(path);
    for (const auto& line : lines) {
        if (line.kind == EnvLine::Kind::KeyValue && line.key == key_str) {
            return line.value;
        }
    }
    if (const char* env = std::getenv(key_str.c_str()); env != nullptr) {
        return std::string(env);
    }
    return std::nullopt;
}

void clear_credential(std::string_view key) {
    const auto path = env_path();
    auto lines = parse(path);
    std::vector<EnvLine> filtered;
    filtered.reserve(lines.size());
    bool changed = false;
    for (auto& line : lines) {
        if (line.kind == EnvLine::Kind::KeyValue && line.key == key) {
            changed = true;
            continue;
        }
        filtered.push_back(std::move(line));
    }
    if (!changed) {
        return;
    }
    write_env_file(path, filtered);
}

void clear_all_credentials() {
    const auto path = env_path();
    std::error_code ec;
    fs::remove(path, ec);
    // Missing file is fine; anything else we swallow (parity with
    // Python reference which logs-and-ignores).
}

std::vector<std::string> list_credential_keys() {
    std::vector<std::string> out;
    const auto path = env_path();
    const auto lines = parse(path);
    for (const auto& line : lines) {
        if (line.kind == EnvLine::Kind::KeyValue) {
            out.push_back(line.key);
        }
    }
    return out;
}

}  // namespace hermes::auth
