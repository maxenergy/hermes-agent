#include "hermes/tools/discover_tools.hpp"
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/image_generation_tool.hpp"
#include "hermes/tools/rl_training_tool.hpp"
#include "hermes/tools/terminal_tool.hpp"
#include "hermes/tools/vision_tool.hpp"
#include "hermes/tools/web_tools.hpp"

#include "hermes/llm/llm_client.hpp"

namespace hermes::tools {

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
}

}  // namespace hermes::tools
