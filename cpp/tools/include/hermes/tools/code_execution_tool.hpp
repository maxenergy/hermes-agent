// Code execution tool — run Python or Bash code snippets in a sandboxed temp file.
#pragma once

#include "hermes/tools/registry.hpp"

namespace hermes::tools {

void register_code_execution_tools(ToolRegistry& registry);

}  // namespace hermes::tools
