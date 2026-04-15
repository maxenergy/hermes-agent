// Depth-port helpers for ``tools/code_execution_tool.py``.  The Python
// implementation owns the RPC wire format, the child-process env
// filter, the "forbidden terminal parameters" list, the tool-call /
// output budgeting, and the schema description assembly.  None of
// those need the full runtime to test — they are pure functions here.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::tools::code_execution::depth {

// ---- Child-env filtering -----------------------------------------------

// Env-var name *prefixes* considered safe to forward into the sandbox
// child process.  Matches ``_SAFE_ENV_PREFIXES`` in Python (PATH, HOME,
// USER, LANG, LC_, TERM, TMPDIR, TMP, TEMP, SHELL, LOGNAME, XDG_,
// PYTHONPATH, VIRTUAL_ENV, CONDA).
const std::vector<std::string>& safe_env_prefixes();

// Substrings that, when present (case-insensitively) in an env var name,
// cause the var to be blocked regardless of prefix match.  Mirrors
// ``_SECRET_SUBSTRINGS``.
const std::vector<std::string>& secret_substrings();

// Return ``true`` when ``name`` contains any case-insensitive secret
// substring — used before the prefix allowlist check.
bool env_name_looks_like_secret(std::string_view name);

// Return ``true`` when ``name`` starts with any of the
// ``safe_env_prefixes()``.  Used by the allowlist path.
bool env_name_has_safe_prefix(std::string_view name);

// Filter an env-var map down to the subset the sandbox child should see.
// ``passthrough`` is the skill/user-configured override callback — when
// it returns ``true`` for a name the var is included unconditionally.
// ``secret_substrings()`` block always wins unless ``passthrough`` says
// yes for that var.  Matches the loop in ``execute_code`` in Python.
using EnvPassthroughFn = bool (*)(const std::string&);

std::unordered_map<std::string, std::string> filter_child_env(
    const std::unordered_map<std::string, std::string>& src,
    EnvPassthroughFn passthrough = nullptr);

// ---- Forbidden terminal params -----------------------------------------

// Return the set of ``terminal()`` keyword args the sandbox must strip
// before forwarding a terminal call (background / pty / force).
const std::unordered_set<std::string>& forbidden_terminal_params();

// Remove any ``forbidden_terminal_params()`` from ``args`` in-place.  No-op
// for non-object values.  Returns the number of keys dropped.
std::size_t strip_forbidden_terminal_params(nlohmann::json& args);

// ---- RPC framing --------------------------------------------------------

struct ParsedRpcFrames {
    // Successfully parsed request frames (each is one JSON object).
    std::vector<nlohmann::json> frames;
    // Bytes left over after the last newline — they belong to the next
    // call.
    std::string residual;
    // Count of frames that failed to parse as JSON.  They are dropped
    // from ``frames`` and reported separately so the caller can emit
    // an error reply without stalling the stream.
    std::size_t parse_errors = 0;
};

// Split a newline-delimited RPC buffer into one JSON per frame.  Blank
// lines are skipped, trailing partial line is returned in ``residual``.
ParsedRpcFrames parse_rpc_frames(std::string_view buffer);

// Encode a single RPC reply for sending back to the sandbox client.  Adds
// the terminating newline the protocol expects.  The input may already
// be a JSON string — we do not re-encode to preserve the handler output.
std::string encode_rpc_reply(std::string_view payload);

// Build the canonical "tool not allowed" reply JSON.  Lists the allowed
// tools alphabetically, matching the Python message verbatim.
std::string build_not_allowed_reply(
    std::string_view tool_name,
    const std::unordered_set<std::string>& allowed);

// Build the "tool call limit reached" reply JSON.
std::string build_limit_reached_reply(int max_calls);

// Build the "invalid RPC request" reply wrapping ``parse_error``.
std::string build_invalid_rpc_reply(std::string_view parse_error);

// ---- Socket-path selection ---------------------------------------------

// Choose the temp directory used for the UDS socket path.  Matches the
// Python policy: ``/tmp`` on macOS (to avoid the 104-byte AF_UNIX limit),
// otherwise ``tmpdir_fallback``.
std::string resolve_socket_tmpdir(std::string_view platform,
                                  std::string_view tmpdir_fallback);

// Build a full UDS socket path given a parent tmpdir and a UUID-like
// token.  Matches ``hermes_rpc_<hex>.sock``.
std::string build_socket_path(std::string_view tmpdir, std::string_view hex_id);

// ---- Tool call logging --------------------------------------------------

// Trim an ``args`` preview for logging.  The Python code uses
// ``str(tool_args)[:80]``.  We match that exactly, preserving the dict
// string layout when the caller is a JSON object.
std::string format_args_preview(const nlohmann::json& args,
                                std::size_t limit = 80);

// Round a duration in seconds to 2 decimals for the tool-call log.
double round_duration_seconds(double seconds);

// ---- Schema description -------------------------------------------------

// Build the ``import`` line of the sandbox example given the enabled tool
// list.  Matches the ``"web_search, terminal"`` preference with a
// "first two alphabetical" fallback, and always appends ``", ..."``.
std::string build_import_examples(
    const std::unordered_set<std::string>& enabled);

// Filter the canonical per-tool doc block to only the enabled tools,
// preserving the canonical display order.
std::vector<std::pair<std::string, std::string>>
filter_tool_doc_lines(const std::unordered_set<std::string>& enabled);

// The canonical per-tool doc block (name, markdown).  Mirrors
// ``_TOOL_DOC_LINES``.
const std::vector<std::pair<std::string, std::string>>&
canonical_tool_doc_lines();

// ---- Sandbox availability ----------------------------------------------

// Return the "not available on Windows" reply used when ``execute_code``
// is called on an unsupported platform.  Matches the Python message.
std::string windows_unsupported_reply();

// Minimum / maximum enforced tool-call budget.  Matches
// ``DEFAULT_MAX_TOOL_CALLS`` clamping in _load_config.
constexpr int kMinToolCalls = 1;
constexpr int kMaxToolCalls = 500;

// Clamp a caller-supplied tool-call budget to the allowed range.
int clamp_max_tool_calls(int requested, int default_value);

}  // namespace hermes::tools::code_execution::depth
