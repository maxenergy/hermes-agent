// Codex-compatible model catalog helpers.
//
// Ports the `hermes_cli/codex_models.py` defaults + forward-compat
// synthetic slugs.  The detection helper here is stricter than the
// general-purpose `is_codex_model()` in model_normalize.hpp — it
// reports true only for models that run against the Codex / ChatGPT
// backend (i.e. `openai-codex` provider), not every GPT-5/Claude
// "code-capable" model.
#pragma once

#include <string>
#include <vector>

namespace hermes::llm {

// Hardcoded fallback list, in priority order.  Matches
// `DEFAULT_CODEX_MODELS` in the Python module.
const std::vector<std::string>& default_codex_models();

// True when `model` is in the curated Codex-compatible list (after
// normalization — leading `openai/` / `openrouter/...` prefixes are
// stripped).  This is the narrow "is this a Codex-backed model?"
// check; use `is_codex_model()` from model_normalize.hpp for the
// broader "is this code-capable?" heuristic.
bool is_codex_backed_model(const std::string& model);

// Expand an ordered list of discovered model IDs with forward-compat
// synthetic slugs.  Mirrors `_add_forward_compat_models()` in the
// Python reference — if newer Codex slugs aren't present but older
// compatible templates are, surface the newer slugs anyway.
std::vector<std::string> add_forward_compat_models(
    const std::vector<std::string>& model_ids);

}  // namespace hermes::llm
