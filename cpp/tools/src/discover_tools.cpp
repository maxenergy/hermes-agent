#include "hermes/tools/discover_tools.hpp"
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/terminal_tool.hpp"

namespace hermes::tools {

void discover_tools() {
    register_file_tools();
    register_terminal_tools();
    // future: register_web_tools(), register_browser_tools(), etc.
}

}  // namespace hermes::tools
