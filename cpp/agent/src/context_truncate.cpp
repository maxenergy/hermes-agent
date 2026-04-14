#include "hermes/agent/context_truncate.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace hermes::agent {

namespace {

std::string format_commas(std::size_t n) {
    std::string raw = std::to_string(n);
    std::string out;
    int cnt = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (cnt > 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++cnt;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

std::string truncate_context_file(const std::string& content,
                                  const std::string& filename,
                                  std::size_t max_chars,
                                  double head_ratio,
                                  double tail_ratio) {
    if (content.size() <= max_chars) return content;

    const std::size_t head_len = static_cast<std::size_t>(
        static_cast<double>(max_chars) * head_ratio);
    const std::size_t tail_len = static_cast<std::size_t>(
        static_cast<double>(max_chars) * tail_ratio);
    const std::size_t dropped = content.size() - head_len - tail_len;

    std::ostringstream os;
    os.write(content.data(), static_cast<std::streamsize>(head_len));
    os << "\n\n[... truncated " << format_commas(dropped) << " chars from "
       << filename << " ...]\n\n";
    os.write(content.data() + content.size() - tail_len,
             static_cast<std::streamsize>(tail_len));
    return os.str();
}

}  // namespace hermes::agent
