#include "hermes/tools/registry.hpp"

#include "hermes/tools/budget_config.hpp"
#include "hermes/tools/tool_result.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

namespace hermes::tools {

namespace {

// Wrap an arbitrary throwing call into ``{"error": "..."}``.
std::string make_error(std::string_view message,
                       std::initializer_list<std::pair<std::string, nlohmann::json>> extra = {}) {
    nlohmann::json obj = nlohmann::json::object();
    obj["error"] = std::string(message);
    for (const auto& kv : extra) {
        obj[kv.first] = kv.second;
    }
    return obj.dump();
}

}  // namespace

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry inst;
    return inst;
}

void ToolRegistry::register_tool(ToolEntry entry) {
    if (entry.name.empty()) {
        throw std::invalid_argument("ToolEntry::name must not be empty");
    }
    std::lock_guard<std::mutex> lk(mu_);
    tools_[entry.name] = std::move(entry);
}

void ToolRegistry::register_toolset_check(std::string toolset, CheckFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    toolset_checks_[std::move(toolset)] = std::move(fn);
}

bool ToolRegistry::deregister(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return false;
    }
    const std::string ts = it->second.toolset;
    tools_.erase(it);
    // Drop the toolset check if no tools remain in that toolset.
    bool has_other = false;
    for (const auto& kv : tools_) {
        if (kv.second.toolset == ts) {
            has_other = true;
            break;
        }
    }
    if (!has_other) {
        toolset_checks_.erase(ts);
    }
    return true;
}

std::string ToolRegistry::dispatch(const std::string& name,
                                   const nlohmann::json& args,
                                   const ToolContext& ctx) {
    // 1. Lookup (under the lock so registration races are safe).
    ToolEntry entry;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return make_error("unknown tool: " + name);
        }
        entry = it->second;  // copy out so we run the handler unlocked
    }

    // 2. check_fn
    if (entry.check_fn) {
        bool ok = false;
        try {
            ok = entry.check_fn();
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            return make_error("tool unavailable: " + name);
        }
    }

    // 3. requires_env
    for (const auto& var : entry.requires_env) {
        const char* val = std::getenv(var.c_str());
        if (val == nullptr || *val == '\0') {
            return make_error("missing required environment variable: " + var);
        }
    }

    // 4. Run the handler under try/catch.
    std::string result;
    try {
        result = entry.handler(args, ctx);
    } catch (const std::exception& e) {
        return make_error(std::string("handler exception: ") + e.what());
    } catch (...) {
        return make_error("handler exception: unknown");
    }

    // 5. Empty / null → {"ok": true}
    if (result.empty()) {
        nlohmann::json ok = nlohmann::json::object();
        ok["ok"] = true;
        result = ok.dump();
    } else {
        // 6. Make sure the result is valid JSON.  If not, wrap it.
        try {
            auto parsed = nlohmann::json::parse(result);
            // If parsed value is not an object, standardize() wraps it
            // so callers always see ``{...}`` at top level.
            if (!parsed.is_object()) {
                parsed = standardize(parsed);
                result = parsed.dump();
            }
        } catch (const nlohmann::json::parse_error&) {
            nlohmann::json wrapped = nlohmann::json::object();
            wrapped["output"] = result;
            result = wrapped.dump();
        }
    }

    // 7. Apply truncation.
    const std::size_t cap =
        entry.max_result_size_chars > 0 ? entry.max_result_size_chars
                                        : DEFAULT_RESULT_SIZE_CHARS;
    return truncate_result(result, cap);
}

std::vector<hermes::llm::ToolSchema> ToolRegistry::get_definitions(
    const std::vector<std::string>& enabled,
    const std::vector<std::string>& disabled) const {
    std::set<std::string> enabled_set(enabled.begin(), enabled.end());
    std::set<std::string> disabled_set(disabled.begin(), disabled.end());

    // Snapshot the table under the lock; we run check_fns afterwards.
    std::vector<ToolEntry> snapshot;
    std::unordered_map<std::string, CheckFn> ts_checks;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snapshot.reserve(tools_.size());
        for (const auto& kv : tools_) {
            snapshot.push_back(kv.second);
        }
        ts_checks = toolset_checks_;
    }

    std::sort(snapshot.begin(), snapshot.end(),
              [](const ToolEntry& a, const ToolEntry& b) {
                  return a.name < b.name;
              });

    std::vector<hermes::llm::ToolSchema> out;
    out.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        if (!enabled_set.empty() && !enabled_set.count(entry.toolset)) {
            continue;
        }
        if (disabled_set.count(entry.toolset)) {
            continue;
        }
        // Toolset-level check.
        auto ts_it = ts_checks.find(entry.toolset);
        if (ts_it != ts_checks.end() && ts_it->second) {
            try {
                if (!ts_it->second()) continue;
            } catch (...) {
                continue;
            }
        }
        // Per-tool check.
        if (entry.check_fn) {
            try {
                if (!entry.check_fn()) continue;
            } catch (...) {
                continue;
            }
        }
        hermes::llm::ToolSchema schema;
        schema.name = entry.name;
        schema.description = entry.description;
        // Schema may store {"parameters": {...}, "description": "..."} from
        // an OpenAI function-call shape, or just the parameters object
        // directly.  Honour both.
        if (entry.schema.is_object()) {
            if (entry.schema.contains("parameters")) {
                schema.parameters = entry.schema.at("parameters");
                if (entry.schema.contains("description") && schema.description.empty()) {
                    schema.description = entry.schema.at("description").get<std::string>();
                }
            } else {
                schema.parameters = entry.schema;
            }
        } else {
            schema.parameters = nlohmann::json::object();
        }
        out.push_back(std::move(schema));
    }
    return out;
}

std::optional<std::string> ToolRegistry::get_toolset_for_tool(
    const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return std::nullopt;
    return it->second.toolset;
}

std::vector<std::string> ToolRegistry::list_tools() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ToolRegistry::list_toolsets() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> uniq;
    for (const auto& kv : tools_) uniq.insert(kv.second.toolset);
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

bool ToolRegistry::has_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return tools_.count(name) > 0;
}

std::string ToolRegistry::get_emoji(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(tool_name);
    if (it == tools_.end() || it->second.emoji.empty()) {
        return "\xE2\x9A\xA1";  // ⚡ — default fallback
    }
    return it->second.emoji;
}

std::size_t ToolRegistry::get_max_result_size(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tools_.find(tool_name);
    if (it != tools_.end() && it->second.max_result_size_chars > 0) {
        return it->second.max_result_size_chars;
    }
    return DEFAULT_RESULT_SIZE_CHARS;
}

bool ToolRegistry::is_toolset_available(const std::string& toolset) const {
    CheckFn fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = toolset_checks_.find(toolset);
        if (it == toolset_checks_.end()) return true;
        fn = it->second;
    }
    if (!fn) return true;
    try {
        return fn();
    } catch (...) {
        return false;
    }
}

std::vector<std::string> ToolRegistry::last_resolved_tool_names() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_resolved_;
}

void ToolRegistry::set_last_resolved_tool_names(std::vector<std::string> names) {
    std::lock_guard<std::mutex> lk(mu_);
    last_resolved_ = std::move(names);
}

void ToolRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    tools_.clear();
    toolset_checks_.clear();
    last_resolved_.clear();
}

std::size_t ToolRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tools_.size();
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

std::string tool_result(const nlohmann::json& data) {
    return standardize(data).dump();
}

std::string tool_result(
    std::initializer_list<std::pair<std::string, nlohmann::json>> kv) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& p : kv) obj[p.first] = p.second;
    return obj.dump();
}

std::string tool_error(
    std::string_view message,
    std::initializer_list<std::pair<std::string, nlohmann::json>> extra) {
    nlohmann::json obj = nlohmann::json::object();
    obj["error"] = std::string(message);
    for (const auto& kv : extra) obj[kv.first] = kv.second;
    return obj.dump();
}

}  // namespace hermes::tools
