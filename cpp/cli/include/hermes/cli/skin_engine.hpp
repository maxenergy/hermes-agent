// CLI theming ("skin") engine — colours, spinner art, branding strings.
//
// Built-in skins: default, ares, mono, slate.  Users may override via a
// YAML file in <HERMES_HOME>/skins/<name>.yaml.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::cli {

struct SkinConfig {
    std::string name;
    std::string description;

    struct Colors {
        std::string banner_border  = "\033[36m";   // cyan
        std::string banner_title   = "\033[1;36m"; // bold cyan
        std::string banner_accent  = "\033[33m";   // yellow
        std::string banner_dim     = "\033[2m";    // dim
        std::string banner_text    = "\033[0m";    // reset
        std::string response_border = "\033[35m";  // magenta
    } colors;

    struct Spinner {
        std::vector<std::string> waiting_faces  = {"(o_o)", "(O_O)", "(o_O)"};
        std::vector<std::string> thinking_faces = {"(._. )", "( ._.)", "( ._.)~"};
        std::vector<std::string> thinking_verbs = {"pondering", "reasoning", "thinking"};
        std::vector<std::pair<std::string, std::string>> wings = {
            {">", "<"}, {">>", "<<"}, {">>>", "<<<"}, {">>>>", "<<<<"}, {">>>>>", "<<<<<"}
        };
    } spinner;

    std::string tool_prefix = "\xe2\x94\x8a";  // "┊" utf-8

    struct Branding {
        std::string agent_name    = "Hermes";
        std::string welcome;
        std::string response_label = " Hermes ";
        std::string prompt_symbol  = "\xe2\x9d\xaf";  // "❯" utf-8
    } branding;
};

// Built-in skins: default, ares, mono, slate.
const std::map<std::string, SkinConfig>& builtin_skins();

// Load a skin by name: checks user YAML first, then builtins, falls back to
// "default".
SkinConfig load_skin(const std::string& name);

// Initialize the global active skin from a Hermes config object (reads
// "cli.skin" key).
void init_skin_from_config(const nlohmann::json& config);

// Global active skin accessors.
const SkinConfig& get_active_skin();
void set_active_skin(const std::string& name);

}  // namespace hermes::cli
