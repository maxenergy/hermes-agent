#include "hermes/tools/tool_result.hpp"

#include <algorithm>
#include <string>

namespace hermes::tools {

std::string truncate_result(std::string_view json_result, std::size_t max_size) {
    if (json_result.size() <= max_size) {
        return std::string(json_result);
    }

    const std::size_t removed = json_result.size() - max_size;
    const std::string marker =
        " [...truncated " + std::to_string(removed) + " chars...]";

    // Try to fit ``head + marker`` inside ``max_size``; if max_size is too
    // small to even hold the marker, emit the marker alone.
    if (max_size <= marker.size()) {
        return marker;
    }

    const std::size_t head_len = max_size - marker.size();
    std::string out;
    out.reserve(max_size);
    out.append(json_result.substr(0, head_len));
    out.append(marker);
    return out;
}

nlohmann::json standardize(const nlohmann::json& raw) {
    if (raw.is_object()) {
        return raw;
    }
    nlohmann::json wrapped = nlohmann::json::object();
    wrapped["output"] = raw;
    return wrapped;
}

}  // namespace hermes::tools
