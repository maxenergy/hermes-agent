// Clarify tool — ask the user a clarifying question via a platform callback.
#pragma once

#include "hermes/tools/registry.hpp"

#include <functional>
#include <string>
#include <vector>

namespace hermes::tools {

// Callback type: receives the question and up to 4 choices, returns the
// user's answer as a string.
using ClarifyCallback = std::function<std::string(
    const std::string& question, const std::vector<std::string>& choices)>;

void set_clarify_callback(ClarifyCallback cb);
void clear_clarify_callback();

void register_clarify_tools(ToolRegistry& registry);

}  // namespace hermes::tools
