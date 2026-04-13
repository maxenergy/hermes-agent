#include "hermes/llm/codex_models.hpp"

#include "hermes/llm/model_normalize.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace hermes::llm {

namespace {

const std::vector<std::string>& default_codex_models_impl() {
    static const std::vector<std::string> kDefaults = {
        "gpt-5.4-mini",
        "gpt-5.4",
        "gpt-5.3-codex",
        "gpt-5.2-codex",
        "gpt-5.1-codex-max",
        "gpt-5.1-codex-mini",
    };
    return kDefaults;
}

// synthetic -> templates.  If `synthetic` is absent but any of its
// templates is present, we inject `synthetic` after the last matching
// template.
const std::vector<std::pair<std::string, std::vector<std::string>>>&
forward_compat_table() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>>
        kTable = {
            {"gpt-5.4-mini", {"gpt-5.3-codex", "gpt-5.2-codex"}},
            {"gpt-5.4", {"gpt-5.3-codex", "gpt-5.2-codex"}},
            {"gpt-5.3-codex", {"gpt-5.2-codex"}},
            {"gpt-5.3-codex-spark", {"gpt-5.3-codex", "gpt-5.2-codex"}},
        };
    return kTable;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

const std::vector<std::string>& default_codex_models() {
    return default_codex_models_impl();
}

bool is_codex_backed_model(const std::string& model) {
    const std::string needle = to_lower(normalize_model_id(model));
    for (const auto& m : default_codex_models_impl()) {
        if (to_lower(m) == needle) return true;
    }
    for (const auto& [synth, _] : forward_compat_table()) {
        if (to_lower(synth) == needle) return true;
    }
    // Any slug that starts with "gpt-5" and contains "codex" is Codex-backed.
    if (needle.rfind("gpt-5", 0) == 0 &&
        needle.find("codex") != std::string::npos) {
        return true;
    }
    return false;
}

std::vector<std::string> add_forward_compat_models(
    const std::vector<std::string>& model_ids) {
    std::vector<std::string> ordered;
    std::unordered_set<std::string> seen;
    ordered.reserve(model_ids.size());
    for (const auto& id : model_ids) {
        if (seen.insert(id).second) {
            ordered.push_back(id);
        }
    }
    for (const auto& [synthetic, templates] : forward_compat_table()) {
        if (seen.count(synthetic)) continue;
        bool any_template = false;
        for (const auto& t : templates) {
            if (seen.count(t)) {
                any_template = true;
                break;
            }
        }
        if (any_template) {
            ordered.push_back(synthetic);
            seen.insert(synthetic);
        }
    }
    return ordered;
}

}  // namespace hermes::llm
