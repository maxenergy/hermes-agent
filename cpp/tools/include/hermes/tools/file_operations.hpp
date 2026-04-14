// File operation helpers — path validation, binary detection, content reading,
// Jupyter notebook rendering, image metadata, encoding detection, MIME sniffing,
// and fuzzy patch application. This header ports the end-user-visible surface
// of Python `tools/file_operations.py` to C++17.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

// Returns true when the path is a sensitive system file that must never be
// written to by the agent (e.g. /etc/passwd, ~/.ssh/authorized_keys).
bool is_sensitive_path(const fs::path& p);

// Returns true when a write to `p` should be denied. Mirrors
// `tools.file_operations._is_write_denied`: combines the hard deny list,
// prefix denies (~/.ssh, ~/.aws, /etc/sudoers.d, ...), and honours the
// optional `HERMES_WRITE_SAFE_ROOT` sandbox environment variable.
bool is_write_denied(const fs::path& p);

// Expand a leading "~" or "~user" component, using $HOME / getpwnam.
// Returns the input unchanged when expansion is not possible.
std::string expand_user(std::string_view path);

// ---------------------------------------------------------------------------
// Binary / image / notebook detection
// ---------------------------------------------------------------------------

// Returns true when the file is likely binary — checks extension first,
// then inspects the first 8 KB of content for NUL / non-printable runs.
bool is_binary_file(const fs::path& p);

// Returns true when the extension indicates an image we can return as
// base64 with metadata (png/jpg/jpeg/gif/webp/bmp/ico).
bool is_image_file(const fs::path& p);

// Returns true when the path ends in `.ipynb`.
bool is_notebook_file(const fs::path& p);

// ---------------------------------------------------------------------------
// MIME / encoding / BOM
// ---------------------------------------------------------------------------

// Best-effort MIME type from extension. Returns "application/octet-stream"
// when unknown. Avoids dragging in libmagic — extension-only is enough for
// the agent's needs and matches Python's fallback behaviour when libmagic
// is not installed.
std::string detect_mime_type(const fs::path& p);

// Inspect the first few bytes to detect BOM-prefixed encodings. Returns
// one of "utf-8", "utf-8-bom", "utf-16le", "utf-16be", "utf-32le",
// "utf-32be" or "unknown" when no BOM is present.
std::string detect_encoding(std::string_view head);

// Strip a leading BOM from `content` in place. Returns the number of bytes
// stripped (0, 2, 3, or 4).
std::size_t strip_bom(std::string& content);

// Normalise CR/LF line endings — replace "\r\n" with "\n" and drop stray
// lone "\r". Returns the normalised string.
std::string normalise_newlines(std::string_view in);

// ---------------------------------------------------------------------------
// Image metadata — magic-byte format detection and dimension extraction
// ---------------------------------------------------------------------------

struct ImageInfo {
    std::string format;       // "png" / "jpeg" / "gif" / "webp" / "bmp" / "ico" / ""
    int width = 0;
    int height = 0;
};

// Parse magic bytes / headers to determine image format and dimensions.
// Returns an empty `format` when the bytes are not a recognised image.
ImageInfo parse_image_info(std::string_view bytes);

// Base64-encode an arbitrary byte buffer (no newlines, RFC 4648).
std::string base64_encode(std::string_view bytes);

// ---------------------------------------------------------------------------
// File content reading
// ---------------------------------------------------------------------------

// Read file content starting at byte `offset`. When `limit` is -1 the entire
// remainder is returned. Throws std::runtime_error on I/O failure.
std::string read_file_content(const fs::path& p, int64_t offset = 0,
                              int64_t limit = -1);

// Read the entire file content. Throws on I/O failure.
std::string read_file_all(const fs::path& p);

// Attributes a caller may want for display or auditing — mode string (rwx),
// size in bytes, and ISO-8601 mtime in UTC.
struct FileStat {
    std::string mode;          // e.g. "rw-r--r--"
    std::int64_t size = 0;
    std::string mtime_iso;     // e.g. "2026-04-13T12:34:56Z"
    bool is_symlink = false;
    std::string symlink_target;
};

FileStat stat_file(const fs::path& p);

// Follow symlinks with cycle detection. Returns the resolved path or an
// empty path when a cycle is detected (max 32 hops).
fs::path resolve_symlink_safe(const fs::path& p);

// ---------------------------------------------------------------------------
// Jupyter notebook
// ---------------------------------------------------------------------------

struct NotebookCell {
    std::string cell_type;     // "code" / "markdown" / "raw"
    std::string source;        // joined source as plain text
    std::vector<std::string> outputs;  // rendered text outputs
    int execution_count = -1;  // -1 when absent
};

// Parse an .ipynb JSON string into an ordered list of cells. Returns an
// empty vector on malformed JSON.
std::vector<NotebookCell> parse_notebook(std::string_view json_text);

// Render a notebook to human-readable text (markdown-ish). Used for
// `read_file` output when the caller asks for a notebook.
std::string render_notebook(const std::vector<NotebookCell>& cells);

// Given an existing notebook JSON and edited cells, emit a new notebook
// JSON preserving metadata. Missing cells keep their original metadata.
std::string edit_notebook(std::string_view original_json,
                          const std::vector<NotebookCell>& edited_cells);

// ---------------------------------------------------------------------------
// Patch / diff
// ---------------------------------------------------------------------------

// Apply a unified-diff hunk list to `original` with fuzzy context matching.
// Returns the patched content; `applied` is set to the number of hunks
// accepted. `err` is populated when a hunk could not be matched.
struct ApplyOptions {
    int fuzz = 3;              // max lines of drift allowed for each hunk
    bool ignore_whitespace = true;
};

std::string apply_unified_diff(std::string_view original,
                               std::string_view diff,
                               int& applied_out,
                               std::string& err_out,
                               ApplyOptions opts = {});

// Simple context-diff / ed-style application helper for the rare cases
// where a model emits those formats. On parse failure, returns the original
// unmodified content and sets `err_out`.
std::string apply_context_or_ed_diff(std::string_view original,
                                     std::string_view diff,
                                     int& applied_out,
                                     std::string& err_out);

// ---------------------------------------------------------------------------
// Line-number rendering
// ---------------------------------------------------------------------------

constexpr int kMaxReadLines = 2000;
constexpr int kMaxLineLength = 2000;
constexpr std::int64_t kMaxReadFileSize = 50 * 1024;   // 50 KiB budget

// Prefix each line with `"%6d|"`-formatted line numbers. Lines longer than
// `kMaxLineLength` are truncated and marked with "... [truncated]".
std::string add_line_numbers(std::string_view content, int start_line = 1);

// ---------------------------------------------------------------------------
// Search budget
// ---------------------------------------------------------------------------

struct SearchBudget {
    int max_matches = 50;
    std::int64_t max_bytes = 4 * 1024 * 1024;   // 4 MiB scanned total
    int max_files = 1000;
};

}  // namespace hermes::tools
