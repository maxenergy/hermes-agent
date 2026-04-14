// ToolContext — helpers for building base64-chunked upload/download shell
// commands that run inside a sandbox environment (Modal, Docker, Daytona,
// local shell, etc.).
//
// The Python equivalent lives in environments/tool_context.py and couples
// directly to model_tools.handle_function_call.  In C++ we keep only the
// pieces that don't need a tool dispatcher — the command builders, result
// parsers, path utilities — so an environment backend can plug them into
// whatever exec path it already has.
//
// Typical use:
//
//     auto plan = tool_context::build_upload_plan(local_bytes, "/work/x.tar");
//     for (auto& cmd : plan.commands) env.exec(cmd);
//     tool_context::ExecResult r = tool_context::parse_terminal_result(json);
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::environments::tool_context {

// ---------------------------------------------------------------------------
// Base64 helpers
// ---------------------------------------------------------------------------

// Encode raw bytes to canonical base64 (no newlines, padded with '=').
std::string base64_encode(const std::uint8_t* data, std::size_t n);
std::string base64_encode(std::string_view bytes);

// Decode base64 back to raw bytes.  Returns std::nullopt on malformed input.
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view encoded);

// ---------------------------------------------------------------------------
// Upload plan
// ---------------------------------------------------------------------------

// The Python upload_file() path either writes a single base64 blob via
// `printf ... | base64 -d > remote` or, for payloads larger than kChunkSize,
// streams chunks into /tmp/_hermes_upload.b64 and then decodes.  The C++
// surface produces the same sequence of shell commands without assuming an
// exec mechanism.
struct UploadPlan {
    std::vector<std::string> commands;  // execute in order
    std::size_t encoded_size = 0;       // length of the base64 payload
    bool chunked = false;
};

inline constexpr std::size_t kChunkSize = 60'000;  // ~60KB per chunk

UploadPlan build_upload_plan(const std::uint8_t* data, std::size_t n,
                              std::string_view remote_path,
                              std::size_t chunk_size = kChunkSize);
UploadPlan build_upload_plan(std::string_view bytes,
                              std::string_view remote_path,
                              std::size_t chunk_size = kChunkSize);

// Helper: is the parent path in Python's "no-op" set `(".", "/")`?  If not,
// callers should inject a `mkdir -p <parent>` before the upload plan.
std::optional<std::string> remote_mkdir_parent(std::string_view remote_path);

// Shell-safe single-quote: wraps `s` in single quotes and escapes any
// embedded single quote as `'\''`.  Used by command builders and by any
// caller that wants to interpolate untrusted paths.
std::string shell_single_quote(std::string_view s);

// ---------------------------------------------------------------------------
// Download plan
// ---------------------------------------------------------------------------

// Download uses a single `base64 <remote>` command; the output is captured
// and fed to decode_download_output() which strips whitespace and decodes.
std::string build_download_command(std::string_view remote_path);

struct DownloadResult {
    bool success = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
};

DownloadResult decode_download_output(std::string_view terminal_output);

// ---------------------------------------------------------------------------
// Directory helpers
// ---------------------------------------------------------------------------

// Build the list of remote commands that enumerate all regular files under
// `remote_dir`.  `find` is used on POSIX; the default returns the full path.
std::string build_remote_find_command(std::string_view remote_dir);

// Split `find` stdout into a sorted list of remote file paths, preserving
// lexical order.  Empty / whitespace-only lines are skipped.
std::vector<std::string> parse_find_output(std::string_view output);

// Given a remote file path and the root remote directory, compute the path
// relative to the root (falling back to the basename if it escapes).
std::string relative_remote_path(std::string_view remote_file,
                                  std::string_view remote_root);

// Enumerate a local directory, producing (absolute, relative) pairs in
// lexical order — matches the Python upload_dir loop.
struct LocalFileEntry {
    std::filesystem::path absolute;
    std::string relative;       // POSIX-style separators for the sandbox
};
std::vector<LocalFileEntry> enumerate_local_dir(const std::filesystem::path& dir);

// ---------------------------------------------------------------------------
// Terminal-result parsing
// ---------------------------------------------------------------------------

struct ExecResult {
    int exit_code = -1;
    std::string output;
};

// Parse the standard `{"exit_code": N, "output": "..."}` JSON returned by
// the Python terminal tool.  Fallback: if the input is not valid JSON, we
// return {-1, <raw-input>}.
ExecResult parse_terminal_result(std::string_view json_or_raw);

}  // namespace hermes::environments::tool_context
