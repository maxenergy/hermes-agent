#include "hermes/core/atomic_io.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace hermes::core::atomic_io {

namespace {
std::atomic<std::uint64_t> g_counter{0};
}  // namespace

bool atomic_write(const fs::path& dst, std::string_view content) noexcept {
    try {
        if (dst.empty()) {
            return false;
        }

        std::error_code ec;
        const auto parent = dst.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                return false;
            }
        }

        const auto pid = static_cast<long>(::getpid());
        const auto counter = g_counter.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream tmp_name;
        tmp_name << dst.filename().string() << ".tmp." << pid << "." << counter;
        const auto tmp = parent.empty() ? fs::path(tmp_name.str())
                                        : parent / tmp_name.str();

        // Use POSIX-level IO so we can fsync before the rename.
        const int fd = ::open(tmp.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                              S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            return false;
        }

        const char* data = content.data();
        std::size_t remaining = content.size();
        while (remaining > 0) {
            const auto written = ::write(fd, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                std::error_code rm_ec;
                fs::remove(tmp, rm_ec);
                return false;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }

        if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
            // EINVAL/ENOTSUP happen on some exotic filesystems — tolerate.
            ::close(fd);
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            return false;
        }
        if (::close(fd) != 0) {
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            return false;
        }

        fs::rename(tmp, dst, ec);
        if (ec) {
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> atomic_read(const fs::path& src) noexcept {
    try {
        std::ifstream in(src, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        if (in.bad()) {
            return std::nullopt;
        }
        return buf.str();
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace hermes::core::atomic_io
