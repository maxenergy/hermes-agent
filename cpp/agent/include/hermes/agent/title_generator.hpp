// Auto-generate a short session title from the first user message.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>
#include <string_view>

namespace hermes::agent {

// Call the auxiliary client with a "summarise in <=8 words" prompt and
// return the trimmed title (capped at 60 chars).  Returns empty string
// on any error.  `aux` may be null — that also returns empty string.
std::string generate_title(hermes::llm::LlmClient* aux,
                           std::string_view model,
                           std::string_view first_user_message);

}  // namespace hermes::agent
