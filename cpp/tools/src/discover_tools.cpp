#include "hermes/tools/discover_tools.hpp"
#include "hermes/tools/browser_backend.hpp"
#include "hermes/tools/cdp_backend.hpp"
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/image_generation_tool.hpp"
#include "hermes/tools/rl_training_tool.hpp"
#include "hermes/tools/terminal_tool.hpp"
#include "hermes/tools/vision_tool.hpp"
#include "hermes/tools/web_tools.hpp"

#include "hermes/llm/llm_client.hpp"

namespace hermes::tools {

namespace {

/// Attempt to launch a CDP browser backend.  If Chrome is not found or
/// launch fails, browser tools simply remain unavailable — this is not an
/// error (Chrome may legitimately not be installed in CLI mode).
void try_init_browser_backend() {
    // Only attempt if no backend is already set (e.g. a test double
    // injected by tests).
    if (get_browser_backend() != nullptr) return;

    auto backend = make_cdp_backend();
    if (backend) {
        set_browser_backend(std::move(backend));
    }
}

}  // namespace

void discover_tools() {
    register_file_tools();
    register_terminal_tools();

    // Wire HTTP-dependent tools with the global transport (may be nullptr
    // when no HTTP backend was compiled in — each tool handles that).
    auto* tp = hermes::llm::get_default_transport();
    register_web_tools(tp);
    register_vision_tools(tp);
    register_image_gen_tools(tp);
    register_rl_tools(tp);

    // Attempt to launch a browser backend for CDP-based browser tools.
    try_init_browser_backend();
}

}  // namespace hermes::tools
