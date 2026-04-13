#include "hermes/tools/debug_helpers.hpp"

#include "hermes/core/logging.hpp"
#include "hermes/tools/registry.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

namespace hermes::tools::debug {

namespace {

// Tri-state so a lazy getenv() read only runs the first time the logger is
// queried after process start.  -1 = unknown, 0 = off, 1 = on.
std::atomic<int> g_enabled{-1};

int resolve_enabled() {
    int cur = g_enabled.load(std::memory_order_acquire);
    if (cur >= 0) return cur;
    const char* v = std::getenv("HERMES_DEBUG_TOOLS");
    int next = 0;
    if (v && *v) {
        std::string s(v);
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        }
        if (s == "1" || s == "true" || s == "yes" || s == "on") next = 1;
    }
    g_enabled.store(next, std::memory_order_release);
    return next;
}

}  // namespace

void enable_tool_call_logging(bool on) {
    g_enabled.store(on ? 1 : 0, std::memory_order_release);
}

bool tool_call_logging_enabled() {
    return resolve_enabled() != 0;
}

void log_tool_call(const std::string& tool_name,
                   const nlohmann::json& args,
                   const std::string& result_preview,
                   std::chrono::milliseconds duration) {
    if (!tool_call_logging_enabled()) return;

    std::ostringstream oss;
    oss << "[tool] " << tool_name
        << " args=" << args.dump()
        << " dur=" << duration.count() << "ms"
        << " result=" << result_preview;
    hermes::core::logging::log_debug(oss.str());
}

std::string dump_registry_state() {
    auto& reg = ToolRegistry::instance();
    std::ostringstream oss;
    oss << "ToolRegistry size=" << reg.size() << "\n";
    auto names = reg.list_tools();
    for (const auto& n : names) {
        auto ts = reg.get_toolset_for_tool(n);
        oss << "  " << n << " [" << (ts ? *ts : std::string("?")) << "]\n";
    }
    oss << "toolsets:\n";
    for (const auto& ts : reg.list_toolsets()) {
        oss << "  " << ts
            << " available=" << (reg.is_toolset_available(ts) ? "1" : "0")
            << "\n";
    }
    return oss.str();
}

}  // namespace hermes::tools::debug
