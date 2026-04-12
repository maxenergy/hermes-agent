// Todo tool — in-memory per-task todo list management.
#pragma once

#include "hermes/tools/registry.hpp"

namespace hermes::tools {

void register_todo_tools(ToolRegistry& registry);

// Reset global todo state (for tests).
void clear_all_todos();

}  // namespace hermes::tools
