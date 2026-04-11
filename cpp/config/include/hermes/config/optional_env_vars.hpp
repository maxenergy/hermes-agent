// Catalog of optional environment variables known to Hermes.
//
// Mirrors `OPTIONAL_ENV_VARS` from `hermes_cli/config.py`.  Names MUST
// match the Python reference exactly — they are part of the public
// configuration contract and are also written into `~/.hermes/.env`.
#pragma once

#include <map>
#include <string>

namespace hermes::config {

struct EnvVarSpec {
    std::string description;
    std::string prompt;
    std::string url;
    std::string category;   // "provider", "tool", "messaging"
    bool password = false;  // true => display as `***` in UIs
};

// Sorted map so iteration order is stable across runs.
const std::map<std::string, EnvVarSpec>& optional_env_vars();

}  // namespace hermes::config
