// Path helpers for the Hermes credential store.
//
// Phase 5 only exposes the path computation — actual mounting of files
// into remote sandboxes lives in Phase 7 (environments).  These helpers
// honour the HERMES_HOME override and degrade gracefully when $HOME is
// unset.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

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

}  // namespace hermes::tools
