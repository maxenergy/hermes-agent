// Entry point for discovering and registering all built-in tools.
#pragma once

namespace hermes::tools {

// Phase 8.1-8.2: file + terminal tools (uses singleton ToolRegistry)
void discover_tools();

// Phase 8 simple tools: memory/todo/clarify/skills/session_search/HA
void register_memory_tools();
void register_todo_tools();
void register_clarify_tools();
void register_skills_tools();
void register_session_search_tools();
void register_homeassistant_tools();

}  // namespace hermes::tools
