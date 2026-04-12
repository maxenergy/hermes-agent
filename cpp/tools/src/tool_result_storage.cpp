#include "hermes/tools/tool_result_storage.hpp"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace hermes::tools {

namespace {

std::string make_uuid() {
    // Tiny UUIDv4 generator — no dependency on libuuid.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    const std::uint64_t a = dist(rng);
    const std::uint64_t b = dist(rng);

    auto hex = [](std::uint64_t v, int width) {
        std::ostringstream os;
        os.width(width);
        os.fill('0');
        os << std::hex << v;
        return os.str();
    };

    // 8-4-4-4-12 with version (4) and variant (8/9/a/b) bits set.
    const std::uint64_t a_hi = (a >> 32) & 0xFFFFFFFFULL;
    const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
    const std::uint64_t b_hi = (b >> 32) & 0xFFFFFFFFULL;
    const std::uint64_t b_lo = b & 0xFFFFFFFFULL;

    const std::uint64_t time_low = a_hi;
    const std::uint64_t time_mid = (a_lo >> 16) & 0xFFFFULL;
    const std::uint64_t time_hi_v = (a_lo & 0x0FFFULL) | 0x4000ULL;  // version 4
    const std::uint64_t clock = ((b_hi >> 16) & 0x3FFFULL) | 0x8000ULL;  // variant
    const std::uint64_t node = ((b_hi & 0xFFFFULL) << 32) | b_lo;

    return hex(time_low, 8) + "-" + hex(time_mid, 4) + "-" +
           hex(time_hi_v, 4) + "-" + hex(clock, 4) + "-" +
           hex(node, 12);
}

constexpr const char* HANDLE_PREFIX = TOOL_RESULT_HANDLE_PREFIX;
constexpr std::size_t HANDLE_PREFIX_LEN = 21;  // strlen("hermes://tool-result/")

bool valid_uuid_chars(std::string_view s) {
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) {
            return false;
        }
    }
    return !s.empty();
}

}  // namespace

ToolResultStorage::ToolResultStorage(std::filesystem::path dir)
    : dir_(std::move(dir)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        throw std::runtime_error("ToolResultStorage: cannot create directory " +
                                 dir_.string() + ": " + ec.message());
    }
}

std::string ToolResultStorage::store(std::string_view content) {
    const std::string uuid = make_uuid();
    const std::filesystem::path file = dir_ / (uuid + ".txt");
    std::ofstream os(file, std::ios::binary | std::ios::trunc);
    if (!os) {
        throw std::runtime_error("ToolResultStorage::store: open failed: " +
                                 file.string());
    }
    os.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!os) {
        throw std::runtime_error("ToolResultStorage::store: write failed: " +
                                 file.string());
    }
    return std::string(HANDLE_PREFIX) + uuid;
}

std::optional<std::string> ToolResultStorage::retrieve(std::string_view handle) {
    if (handle.size() <= HANDLE_PREFIX_LEN) return std::nullopt;
    if (handle.substr(0, HANDLE_PREFIX_LEN) != HANDLE_PREFIX) {
        return std::nullopt;
    }
    const std::string_view uuid = handle.substr(HANDLE_PREFIX_LEN);
    if (!valid_uuid_chars(uuid)) return std::nullopt;

    const std::filesystem::path file = dir_ / (std::string(uuid) + ".txt");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream is(file, std::ios::binary);
    if (!is) return std::nullopt;
    std::ostringstream buf;
    buf << is.rdbuf();
    return buf.str();
}

std::size_t ToolResultStorage::cleanup_older_than(std::chrono::seconds age) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return 0;

    const auto now = std::filesystem::file_time_type::clock::now();
    std::size_t removed = 0;
    for (auto it = std::filesystem::directory_iterator(dir_, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto ft = it->last_write_time(ec);
        if (ec) continue;
        if (now - ft > age) {
            std::error_code rm_ec;
            std::filesystem::remove(it->path(), rm_ec);
            if (!rm_ec) ++removed;
        }
    }
    return removed;
}

}  // namespace hermes::tools
