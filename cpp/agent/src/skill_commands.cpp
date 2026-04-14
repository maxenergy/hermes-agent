#include "hermes/agent/skill_commands.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::agent::skill_commands {

namespace {

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string first_line(const std::string& s) {
    std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string trim_hyphens(std::string s) {
    while (!s.empty() && s.front() == '-') s.erase(s.begin());
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s;
}

std::string collapse_runs(const std::string& s, char c) {
    std::string out;
    out.reserve(s.size());
    bool prev = false;
    for (char ch : s) {
        if (ch == c) {
            if (!prev) out.push_back(ch);
            prev = true;
        } else {
            out.push_back(ch);
            prev = false;
        }
    }
    return out;
}

}  // namespace

std::string make_plan_slug(const std::string& heading) {
    std::string source = trim(first_line(heading));
    if (source.empty()) return "conversation-plan";
    std::string lowered = lower_copy(source);
    // Replace non-[a-z0-9] with '-'.
    std::string cleaned;
    cleaned.reserve(lowered.size());
    for (char c : lowered) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) cleaned.push_back(c);
        else cleaned.push_back('-');
    }
    cleaned = collapse_runs(cleaned, '-');
    cleaned = trim_hyphens(std::move(cleaned));
    if (cleaned.empty()) return "conversation-plan";

    // Keep up to 8 words, then cap at 48 chars.
    std::vector<std::string> words;
    std::string cur;
    for (char c : cleaned) {
        if (c == '-') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.size() > 8) words.resize(8);

    std::string joined;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) joined.push_back('-');
        joined += words[i];
    }
    if (joined.size() > 48) joined.resize(48);
    joined = trim_hyphens(std::move(joined));
    return joined.empty() ? "conversation-plan" : joined;
}

std::filesystem::path build_plan_path(const std::string& user_instruction,
                                      std::chrono::system_clock::time_point now) {
    using clock = std::chrono::system_clock;
    auto ts = (now == clock::time_point{}) ? clock::now() : now;
    std::time_t raw = clock::to_time_t(ts);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    std::ostringstream ts_buf;
    ts_buf << std::put_time(&tm, "%Y-%m-%d_%H%M%S");

    std::string slug = make_plan_slug(user_instruction);
    std::filesystem::path p = std::filesystem::path(".hermes") / "plans" /
                              (ts_buf.str() + "-" + slug + ".md");
    return p;
}

std::string sanitise_skill_slug(const std::string& name) {
    std::string lowered = lower_copy(name);
    std::string out;
    out.reserve(lowered.size());
    for (char c : lowered) {
        unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || uc == '-') {
            out.push_back(c);
        } else {
            out.push_back('-');
        }
    }
    out = collapse_runs(out, '-');
    out = trim_hyphens(std::move(out));
    return out;
}

}  // namespace hermes::agent::skill_commands
