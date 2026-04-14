// Pure formatting helpers for insights reports.
#include "hermes/agent/insights_format.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::agent::insights_format {

std::string format_duration(double seconds) {
    long s = std::max<long>(0, static_cast<long>(seconds));
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) {
        long m = s / 60;
        long sec = s % 60;
        if (sec == 0) return std::to_string(m) + "m";
        return std::to_string(m) + "m " + std::to_string(sec) + "s";
    }
    if (s < 86400) {
        long h = s / 3600;
        long m = (s % 3600) / 60;
        if (m == 0) return std::to_string(h) + "h";
        return std::to_string(h) + "h " + std::to_string(m) + "m";
    }
    long d = s / 86400;
    long h = (s % 86400) / 3600;
    if (h == 0) return std::to_string(d) + "d";
    return std::to_string(d) + "d " + std::to_string(h) + "h";
}

std::string format_duration_long(double seconds) {
    long s = std::max<long>(0, static_cast<long>(seconds));
    if (s < 60) {
        return std::to_string(s) + " seconds";
    }
    if (s < 3600) {
        long m = s / 60;
        return std::to_string(m) + (m == 1 ? " minute" : " minutes");
    }
    if (s < 86400) {
        double h = static_cast<double>(s) / 3600.0;
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(1);
        os << h;
        return os.str() + " hours";
    }
    double d = static_cast<double>(s) / 86400.0;
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << d;
    return os.str() + " days";
}

std::string format_with_commas(long long n) {
    std::string raw = std::to_string(n);
    bool neg = !raw.empty() && raw.front() == '-';
    std::string body = neg ? raw.substr(1) : raw;
    std::string out;
    int cnt = 0;
    for (auto it = body.rbegin(); it != body.rend(); ++it) {
        if (cnt > 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++cnt;
    }
    std::reverse(out.begin(), out.end());
    if (neg) out.insert(out.begin(), '-');
    return out;
}

std::string short_session_id(const std::string& id) {
    return id.size() > 16 ? id.substr(0, 16) : id;
}

std::string render_bar_chart(const std::vector<BarRow>& rows,
                             int width,
                             long long max_value) {
    if (rows.empty()) return {};
    long long max_v = max_value;
    if (max_v < 0) {
        max_v = 0;
        for (const auto& r : rows) {
            if (r.value > max_v) max_v = r.value;
        }
    }
    // Longest label for alignment.
    std::size_t label_width = 0;
    for (const auto& r : rows) {
        label_width = std::max(label_width, r.label.size());
    }
    // Longest value for right-align.
    std::size_t count_width = 0;
    for (const auto& r : rows) {
        count_width = std::max(count_width,
                               std::to_string(r.value).size());
    }

    std::string out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::string label = r.label;
        label.resize(label_width, ' ');
        out += label;
        out += "  ";
        int fill = 0;
        if (max_v > 0) {
            fill = static_cast<int>(
                std::floor(static_cast<double>(r.value) * width /
                           static_cast<double>(max_v)));
            fill = std::clamp(fill, 0, width);
        }
        for (int k = 0; k < fill; ++k) out += "█";
        for (int k = fill; k < width; ++k) out += ' ';
        out += "  ";
        std::string n = std::to_string(r.value);
        if (n.size() < count_width) {
            out.append(count_width - n.size(), ' ');
        }
        out += n;
        if (i + 1 < rows.size()) out += '\n';
    }
    return out;
}

std::string format_count_short(long long n) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    if (n >= 1000000) {
        os.precision(1);
        os << (static_cast<double>(n) / 1000000.0) << "M";
    } else if (n >= 1000) {
        os.precision(1);
        os << (static_cast<double>(n) / 1000.0) << "K";
    } else {
        os << n;
    }
    return os.str();
}

namespace {

std::tm gmtime_safe(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

std::string format_date_(std::int64_t epoch_seconds, const char* fmt) {
    if (epoch_seconds <= 0) return {};
    std::tm tm = gmtime_safe(static_cast<std::time_t>(epoch_seconds));
    std::ostringstream os;
    os << std::put_time(&tm, fmt);
    return os.str();
}

}  // namespace

std::string format_date_long(std::int64_t epoch_seconds) {
    return format_date_(epoch_seconds, "%b %d, %Y");
}

std::string format_date_short(std::int64_t epoch_seconds) {
    return format_date_(epoch_seconds, "%b %d");
}

}  // namespace hermes::agent::insights_format
