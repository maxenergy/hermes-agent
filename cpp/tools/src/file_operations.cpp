#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/binary_extensions.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

// Hardcoded blocklist of sensitive paths that must not be written.
const std::vector<std::string>& sensitive_suffixes() {
    static const std::vector<std::string> v = {
        "/etc/passwd",
        "/etc/shadow",
        "/etc/sudoers",
        "/.ssh/authorized_keys",
        "/.ssh/id_rsa",
        "/.ssh/id_ed25519",
        "/.ssh/id_ecdsa",
        "/.ssh/id_dsa",
    };
    return v;
}

}  // namespace

bool is_sensitive_path(const fs::path& p) {
    std::string canonical;
    try {
        // Resolve symlinks / relative components so the blocklist can't be
        // bypassed with ../.. tricks. Fall back to lexically_normal() when
        // the file doesn't exist yet (write_file creates it).
        if (fs::exists(p)) {
            canonical = fs::canonical(p).string();
        } else {
            canonical = fs::absolute(p).lexically_normal().string();
        }
    } catch (...) {
        canonical = fs::absolute(p).lexically_normal().string();
    }

    for (const auto& suffix : sensitive_suffixes()) {
        if (canonical.size() >= suffix.size() &&
            canonical.compare(canonical.size() - suffix.size(),
                              suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

bool is_binary_file(const fs::path& p) {
    if (is_binary_extension(p)) return true;

    // Peek at first 8 KB to check for NUL bytes.
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    char buf[8192];
    in.read(buf, sizeof(buf));
    auto n = in.gcount();
    return is_likely_binary_content(std::string_view(buf, static_cast<std::size_t>(n)));
}

std::string read_file_content(const fs::path& p, int64_t offset, int64_t limit) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file: " + p.string());
    }

    if (offset > 0) {
        in.seekg(offset);
        if (!in) {
            throw std::runtime_error("seek past end of file: " + p.string());
        }
    }

    if (limit < 0) {
        // Read remainder.
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    std::string buf(static_cast<std::size_t>(limit), '\0');
    in.read(buf.data(), limit);
    buf.resize(static_cast<std::size_t>(in.gcount()));
    return buf;
}

}  // namespace hermes::tools
