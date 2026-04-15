// Credential file passthrough — registry of files mounted into remote sandboxes.
//
// Mirrors the Python ``tools/credential_files.py`` module:
//
//   * Skill modules and gateway adapters call ``register_credential_file()``
//     to declare additional files (e.g. service account JSONs) that must
//     follow the conversation into Docker / Modal / Daytona sandboxes.
//   * Remote backends call ``get_credential_file_mounts()`` (path mounts)
//     and ``iter_skills_files()`` / ``iter_cache_files()`` (per-file
//     uploads) at sandbox creation time.
//   * Path containment helpers reject absolute paths and any
//     ``../`` traversal that would escape ``HERMES_HOME``.
//
// Everything in this module is process-global state, scoped per session by
// ``clear_credential_files()`` calls in the gateway pipeline.  That mirrors
// the Python ContextVar-based implementation closely enough for tests; a
// future refactor may swap the global map for a thread-local context.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tools {

// ---- Path helpers ---------------------------------------------------------

// ``$HERMES_HOME`` if set, otherwise ``$HOME/.hermes``.
std::filesystem::path hermes_home();

// ``<hermes_home>/.env``
std::filesystem::path hermes_env_file();

// ``<hermes_home>/.credentials``
std::filesystem::path hermes_credentials_dir();

// ``<hermes_home>/.credentials/<relative>``
std::filesystem::path credential_path(std::string_view relative);

// Walk hermes_credentials_dir() and return every regular file underneath.
// Returns an empty vector if the directory does not exist.
std::vector<std::filesystem::path> list_credential_files();

// ---- Registry of mounted credential files ---------------------------------

struct CredentialMount {
    std::string host_path;
    std::string container_path;
};

// Register a single credential file relative to ``HERMES_HOME``.  Returns
// ``true`` if the file exists on disk, is contained inside the sandbox
// root, and was registered.  Rejects absolute paths and ``..`` traversal.
bool register_credential_file(std::string_view relative_path,
                              std::string_view container_base = "/root/.hermes");

// Bulk register from a list of skill frontmatter entries.  Each item is
// either a plain string (relative path) or a JSON object with a ``path``
// or ``name`` key.  Returns the relative paths that did NOT register.
std::vector<std::string> register_credential_files(
    const nlohmann::json& entries,
    std::string_view container_base = "/root/.hermes");

// Reset the registry — typically called once per session reset.
void clear_credential_files();

// All registered credential mounts that still exist on disk, plus any
// declared in the user config under ``terminal.credential_files``.  Each
// entry has ``host_path`` and ``container_path`` keys.
std::vector<CredentialMount> get_credential_file_mounts();

// ---- Skills directory passthrough ----------------------------------------

// Mount the local ``HERMES_HOME/skills`` directory plus any external
// skill directories.  When symlinks are present a sanitized copy is
// produced in a temp directory; otherwise the original path is returned.
std::vector<CredentialMount> get_skills_directory_mount(
    std::string_view container_base = "/root/.hermes");

// Per-file enumeration of every skill file (preferred for backends like
// Daytona/Modal that upload files one-by-one).  Skips symlinks entirely.
std::vector<CredentialMount> iter_skills_files(
    std::string_view container_base = "/root/.hermes");

// ---- Cache directory passthrough -----------------------------------------

// Mount the four cache subdirectories (documents, images, audio,
// screenshots) into the remote sandbox.
std::vector<CredentialMount> get_cache_directory_mounts(
    std::string_view container_base = "/root/.hermes");

// Per-file enumeration of every cache file.  Used by Modal-style
// backends that upload individual files.
std::vector<CredentialMount> iter_cache_files(
    std::string_view container_base = "/root/.hermes");

// ---- Internal helpers exposed for unit tests -----------------------------

// Returns the resolved host path if ``relative`` is contained inside
// ``hermes_home()`` and refers to a regular file; otherwise an empty
// optional.  Logged warnings flag rejected absolute paths and
// ``..`` traversal.
std::optional<std::filesystem::path> resolve_contained_path(
    std::string_view relative);

// Return ``true`` if any element under ``root`` is a symlink.  Stops at
// the first hit for performance.  Errors are treated as "no symlinks".
bool directory_contains_symlinks(const std::filesystem::path& root);

// Return a sanitized copy of ``skills_dir`` if it contains symlinks,
// otherwise return ``skills_dir`` itself.  The temp dir is cached and
// recreated on each call (matches the Python behaviour).
std::filesystem::path safe_skills_path(const std::filesystem::path& skills_dir);

// Number of currently registered (in-memory) credential files.  Test
// helper.
std::size_t registered_credential_count();

}  // namespace hermes::tools
