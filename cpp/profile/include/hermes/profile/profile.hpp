// Profile manager — full C++17 port of hermes_cli/profiles.py.
//
// Each profile is an isolated HERMES_HOME directory with its own
// config.yaml, .env, memory, sessions, skills, gateway, cron, and logs.
// Profiles live under `<default_hermes_root>/profiles/<name>/`.
//
// CRITICAL INVARIANT (matches Python reference):
//   `get_profiles_root()` is ALWAYS HOME-anchored, not HERMES_HOME-anchored.
//   This lets `hermes -p coder profile list` still enumerate every profile
//   even after the user switches to `~/.hermes/profiles/coder` as the
//   active HERMES_HOME.  Do not "simplify" this to use get_hermes_home().
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::profile {

// ---------------------------------------------------------------------------
// argv preparse / HERMES_HOME override
// ---------------------------------------------------------------------------

// Set `HERMES_HOME` to the target profile directory.  Must be called
// BEFORE any code that reads `hermes::core::path::get_hermes_home()`.
//
// - `std::nullopt` or empty string ⇒ default profile (leaves HERMES_HOME
//   untouched; profile_name="" is a no-op).
// - Any other name ⇒ sets HERMES_HOME to
//   `<default_hermes_root>/profiles/<name>` and creates the directory
//   if it does not yet exist.
void apply_profile_override(std::optional<std::string> profile_name);

// Scan @p argv for an early `--profile=NAME` / `--profile NAME` / `-p NAME`
// pair and return the name (if any).  The matched tokens are *removed*
// from argv by shifting remaining arguments left and decrementing argc
// in place, so downstream subcommand parsers see a clean slice.
std::optional<std::string> preparse_profile_argv(int& argc, char* argv[]);

// Resolve a profile name to a HERMES_HOME path string.  Throws if the
// profile does not exist (except for "default").
std::string resolve_profile_env(std::string_view profile_name);

// ---------------------------------------------------------------------------
// Constants (mirror Python module frozensets)
// ---------------------------------------------------------------------------

const std::vector<std::string>& profile_dirs();
const std::vector<std::string>& clone_config_files();
const std::vector<std::string>& clone_subdir_files();
const std::vector<std::string>& clone_all_strip();
bool is_default_export_excluded_root(std::string_view entry);
bool is_reserved_name(std::string_view name);
bool is_hermes_subcommand(std::string_view name);

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::filesystem::path get_profiles_root();
std::filesystem::path get_default_hermes_home();
std::filesystem::path get_active_profile_path();
std::filesystem::path get_wrapper_dir();

// ---------------------------------------------------------------------------
// Validation / name helpers
// ---------------------------------------------------------------------------

bool is_valid_profile_name(std::string_view name);
void validate_profile_name(std::string_view name);
std::filesystem::path get_profile_dir(std::string_view name);
bool profile_exists(std::string_view name);

// ---------------------------------------------------------------------------
// Alias / wrapper script management
// ---------------------------------------------------------------------------

std::string check_alias_collision(std::string_view name);
bool is_wrapper_dir_in_path();
std::filesystem::path create_wrapper_script(std::string_view name);
bool remove_wrapper_script(std::string_view name);

// ---------------------------------------------------------------------------
// ProfileInfo / listing
// ---------------------------------------------------------------------------

struct ProfileInfo {
    std::string name;
    std::filesystem::path path;
    bool is_default = false;
    bool gateway_running = false;
    std::string model;
    std::string provider;
    bool has_env = false;
    int skill_count = 0;
    std::filesystem::path alias_path;
};

void read_config_model(const std::filesystem::path& profile_dir,
                       std::string& out_model, std::string& out_provider);
bool check_gateway_running(const std::filesystem::path& profile_dir);
int count_skills(const std::filesystem::path& profile_dir);

std::vector<ProfileInfo> list_profile_infos();
std::vector<std::string> list_profiles();

// ---------------------------------------------------------------------------
// CRUD operations
// ---------------------------------------------------------------------------

struct CreateOptions {
    std::string clone_from;
    bool clone_all = false;
    bool clone_config = false;
    bool no_alias = false;
};

// Full-featured create (mirrors Python create_profile()).
std::filesystem::path create_profile_ex(std::string_view name,
                                        const CreateOptions& opts);

// Simple create (no cloning).  Back-compat for existing callers/tests.
void create_profile(std::string_view name);

// Interactive delete (confirmation unless yes=true).
std::filesystem::path delete_profile_ex(std::string_view name, bool yes);

// Simple delete (no confirmation, no service cleanup).  Back-compat.
void delete_profile(std::string_view name);

// Rename a profile.  Updates wrapper script + active_profile.
std::filesystem::path rename_profile(std::string_view old_name,
                                     std::string_view new_name);

// ---------------------------------------------------------------------------
// Active profile (sticky)
// ---------------------------------------------------------------------------

std::string get_active_profile();
void set_active_profile(std::string_view name);
std::string get_active_profile_name();

// ---------------------------------------------------------------------------
// Export / Import
// ---------------------------------------------------------------------------

bool default_export_ignored(const std::filesystem::path& root_dir,
                            const std::filesystem::path& directory,
                            std::string_view entry);
std::vector<std::string> normalize_profile_archive_parts(
    std::string_view member_name);
std::filesystem::path export_profile(std::string_view name,
                                     const std::filesystem::path& output_path);
std::filesystem::path import_profile(const std::filesystem::path& archive_path,
                                     std::string_view name = {});

// ---------------------------------------------------------------------------
// Gateway cleanup helpers
// ---------------------------------------------------------------------------

void cleanup_gateway_service(std::string_view name,
                             const std::filesystem::path& profile_dir);
void stop_gateway_process(const std::filesystem::path& profile_dir);

// ---------------------------------------------------------------------------
// Tab completion script generation
// ---------------------------------------------------------------------------

std::string generate_bash_completion();
std::string generate_zsh_completion();

// ---------------------------------------------------------------------------
// Interactive CLI wizard helpers — called from cmd_profile().
// ---------------------------------------------------------------------------

int wizard_create(std::string_view name);
int print_profile_table(const std::vector<ProfileInfo>& infos);
int print_show(std::string_view name);

}  // namespace hermes::profile
