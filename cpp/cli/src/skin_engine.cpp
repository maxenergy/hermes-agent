#include "hermes/cli/skin_engine.hpp"

#include <mutex>

namespace hermes::cli {

namespace {

SkinConfig make_default_skin() {
    SkinConfig s;
    s.name = "default";
    s.description = "Standard Hermes theme";
    s.branding.welcome = "Welcome to Hermes — your AI agent.";
    return s;
}

SkinConfig make_ares_skin() {
    SkinConfig s;
    s.name = "ares";
    s.description = "Bold red war-god theme";
    s.colors.banner_border  = "\033[31m";
    s.colors.banner_title   = "\033[1;31m";
    s.colors.banner_accent  = "\033[91m";
    s.colors.response_border = "\033[31m";
    s.spinner.thinking_faces = {"(>_<)", "(X_X)", "(>_>)"};
    s.spinner.thinking_verbs = {"battling", "conquering", "strategising"};
    s.branding.agent_name    = "Ares";
    s.branding.welcome       = "Ares stands ready.";
    s.branding.response_label = " Ares ";
    s.branding.prompt_symbol  = "\xe2\x9a\x94";  // crossed swords ⚔
    return s;
}

SkinConfig make_mono_skin() {
    SkinConfig s;
    s.name = "mono";
    s.description = "Monochrome — no colour codes";
    s.colors.banner_border   = "";
    s.colors.banner_title    = "";
    s.colors.banner_accent   = "";
    s.colors.banner_dim      = "";
    s.colors.banner_text     = "";
    s.colors.response_border = "";
    s.branding.welcome = "Welcome to Hermes (mono).";
    return s;
}

SkinConfig make_slate_skin() {
    SkinConfig s;
    s.name = "slate";
    s.description = "Cool blue-grey theme";
    s.colors.banner_border  = "\033[34m";
    s.colors.banner_title   = "\033[1;34m";
    s.colors.banner_accent  = "\033[94m";
    s.colors.response_border = "\033[34m";
    s.branding.welcome = "Hermes (slate) ready.";
    return s;
}

// Global active skin + mutex.
std::mutex& skin_mutex() {
    static std::mutex mu;
    return mu;
}

SkinConfig& mutable_active_skin() {
    static SkinConfig skin = make_default_skin();
    return skin;
}

}  // namespace

const std::map<std::string, SkinConfig>& builtin_skins() {
    static const std::map<std::string, SkinConfig> skins = {
        {"default", make_default_skin()},
        {"ares",    make_ares_skin()},
        {"mono",    make_mono_skin()},
        {"slate",   make_slate_skin()},
    };
    return skins;
}

SkinConfig load_skin(const std::string& name) {
    // TODO: check <HERMES_HOME>/skins/<name>.yaml first.
    const auto& skins = builtin_skins();
    auto it = skins.find(name);
    if (it != skins.end()) return it->second;
    // Fallback to default.
    return skins.at("default");
}

void init_skin_from_config(const nlohmann::json& config) {
    std::string name = "default";
    if (config.contains("cli") && config["cli"].contains("skin")) {
        name = config["cli"]["skin"].get<std::string>();
    }
    set_active_skin(name);
}

const SkinConfig& get_active_skin() {
    std::lock_guard<std::mutex> lk(skin_mutex());
    return mutable_active_skin();
}

void set_active_skin(const std::string& name) {
    auto skin = load_skin(name);
    std::lock_guard<std::mutex> lk(skin_mutex());
    mutable_active_skin() = std::move(skin);
}

}  // namespace hermes::cli
