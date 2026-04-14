// pairing — C++ port of hermes_cli/pairing.py.
//
// The Python CLI exposes four subcommands on top of the gateway's
// PairingStore:
//
//   hermes pairing list                    -> list pending + approved users
//   hermes pairing approve <platform> <code>
//   hermes pairing revoke <platform> <user_id>
//   hermes pairing clear-pending
//
// The existing C++ `hermes::gateway::PairingStore` only exposes
// `approve_code` — this module adds the plain-file introspection + mutation
// helpers needed by the CLI.
//
// All state lives in `<HERMES_HOME>/pairing/`:
//   approved_<platform>.json   -> array of { user_id, user_name }
//   pending_<platform>.json    -> array of { code, user_id, user_name, created_at }
//
// The helper functions here are pure I/O over those files — no locking
// (the CLI is single-threaded) and no cross-platform gateway sharing.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace hermes::cli::pairing {

struct PendingEntry {
    std::string platform;
    std::string code;
    std::string user_id;
    std::string user_name;
    std::chrono::system_clock::time_point created_at;

    // Age in minutes from `now`.
    long age_minutes(std::chrono::system_clock::time_point now =
                         std::chrono::system_clock::now()) const;
};

struct ApprovedEntry {
    std::string platform;
    std::string user_id;
    std::string user_name;
};

// The set of platforms the CLI knows about. Mirrors the Python
// `PLATFORMS` dict in skills_config.py / gateway.pairing.
std::vector<std::string> known_platforms();

// Canonicalise an input platform: lowercased, trimmed. Returns empty
// string when the input is empty.
std::string canonical_platform(std::string_view platform);

// Canonicalise a pairing code: uppercased, trimmed.
std::string canonical_code(std::string_view code);

// Resolve `<HERMES_HOME>/pairing/` (create if missing).
std::filesystem::path default_pairing_dir();

// Path to the pending / approved JSON files for a given platform.
std::filesystem::path pending_path(const std::filesystem::path& dir,
                                   const std::string& platform);
std::filesystem::path approved_path(const std::filesystem::path& dir,
                                    const std::string& platform);

// --- Low-level I/O ---------------------------------------------------------

// Read pending / approved arrays from the raw JSON file. Returns an empty
// list on missing file or parse failure.
std::vector<PendingEntry> read_pending(const std::filesystem::path& dir,
                                       const std::string& platform);
std::vector<ApprovedEntry> read_approved(const std::filesystem::path& dir,
                                         const std::string& platform);

// Write pending / approved arrays back to disk, overwriting the file
// atomically (temp file + rename).
bool write_pending(const std::filesystem::path& dir,
                   const std::string& platform,
                   const std::vector<PendingEntry>& entries);
bool write_approved(const std::filesystem::path& dir,
                    const std::string& platform,
                    const std::vector<ApprovedEntry>& entries);

// --- High-level operations -------------------------------------------------

// Enumerate every pending entry across every known platform (or only the
// platforms whose pending file currently exists).
std::vector<PendingEntry> list_pending(const std::filesystem::path& dir);

// Enumerate every approved entry across every platform.
std::vector<ApprovedEntry> list_approved(const std::filesystem::path& dir);

// Remove a user from the approved list for `platform`. Returns true when
// a removal actually happened.
bool revoke(const std::filesystem::path& dir,
            const std::string& platform,
            const std::string& user_id);

// Clear all pending codes across all platforms. Returns the number of
// entries that were dropped.
std::size_t clear_pending(const std::filesystem::path& dir);

// Move one pending entry into approved. Returns the promoted entry (or
// nullopt when the code is unknown / expired).
std::optional<ApprovedEntry> approve(const std::filesystem::path& dir,
                                     const std::string& platform,
                                     const std::string& code,
                                     std::chrono::seconds ttl =
                                         std::chrono::hours(1));

// --- Rendering -------------------------------------------------------------

// Print a formatted "pending" table (header + rows) to `out`. Used by
// `hermes pairing list`. Returns the number of rows rendered.
std::size_t render_pending_table(std::ostream& out,
                                 const std::vector<PendingEntry>& entries);

// Print a formatted "approved" table to `out`. Returns rows rendered.
std::size_t render_approved_table(std::ostream& out,
                                  const std::vector<ApprovedEntry>& entries);

// Full dispatch: reads dir, prints both tables; returns 0 on success.
int cmd_list(const std::filesystem::path& dir, std::ostream& out);

// --- CLI entry point -------------------------------------------------------

// Top-level dispatcher used by main_entry.cpp. Mirrors
// `pairing_command(args)` in Python.
int dispatch(int argc, char** argv);

}  // namespace hermes::cli::pairing
