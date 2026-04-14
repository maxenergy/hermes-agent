// RL training tools — depth port of ``tools/rl_training_tool.py``.
//
// The Python original does two things: (1) call a remote trainer API
// (preferred in a hosted Hermes deployment), and (2) drive a local
// tinker-atropos checkout via subprocess.  The C++ port provides both,
// with the local path mirrored by ``RlLocalRegistry``.  The tools fall
// back to the local registry when the HTTP transport is absent.

#include "hermes/tools/rl_training_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace hermes::tools {

namespace {

// ---------------------------------------------------------------------------
// Environment variable helpers — mirror Python's os.getenv() semantics.
// ---------------------------------------------------------------------------
std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

bool rl_http_env_available() {
    return !getenv_str("NOUS_RL_API_URL").empty() &&
           !getenv_str("NOUS_RL_API_KEY").empty();
}

bool rl_local_env_available() {
    // Local mode needs a tinker-atropos root and (at minimum) TINKER_API_KEY.
    auto root = getenv_str("TINKER_ATROPOS_ROOT");
    if (root.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(root, ec);
}

bool rl_any_available() {
    return rl_http_env_available() || rl_local_env_available();
}

std::string api_url() { return getenv_str("NOUS_RL_API_URL"); }
std::string api_key() { return getenv_str("NOUS_RL_API_KEY"); }

std::unordered_map<std::string, std::string> auth_headers() {
    return {{"Authorization", "Bearer " + api_key()},
            {"Content-Type", "application/json"}};
}

std::string http_get(hermes::llm::HttpTransport* tp, const std::string& url) {
    auto resp = tp->post_json(url, auth_headers(), "");
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return tool_error("RL API error (HTTP " +
                          std::to_string(resp.status_code) + "): " + resp.body);
    }
    return tool_result(nlohmann::json::parse(resp.body));
}

std::string http_post(hermes::llm::HttpTransport* tp,
                      const std::string& url,
                      const nlohmann::json& body) {
    auto resp = tp->post_json(url, auth_headers(), body.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return tool_error("RL API error (HTTP " +
                          std::to_string(resp.status_code) + "): " + resp.body);
    }
    return tool_result(nlohmann::json::parse(resp.body));
}

std::string http_patch(hermes::llm::HttpTransport* tp,
                       const std::string& url,
                       const nlohmann::json& body) {
    // HttpTransport only has post_json — we use it for PATCH as well.
    return http_post(tp, url, body);
}

// Random 8-char run id (hex), mirrors uuid.uuid4()[:8] in Python.
std::string random_run_id() {
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mu);
    std::uniform_int_distribution<uint64_t> dist;
    auto v = dist(rng);
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x",
                  static_cast<unsigned int>(v & 0xffffffff));
    return buf;
}

// Format system_clock::time_point as YYYYMMDD-HHMMSS in local time.
std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Minimal YAML dumper — the RL config is strictly scalars + nested maps
// + arrays of maps.  Enough depth for the locked/user merge.
void dump_yaml(std::ostringstream& os, const nlohmann::json& v,
               int indent) {
    auto pad = [&](int n) { return std::string(n, ' '); };

    if (v.is_null()) {
        os << "null";
    } else if (v.is_boolean()) {
        os << (v.get<bool>() ? "true" : "false");
    } else if (v.is_number()) {
        os << v.dump();
    } else if (v.is_string()) {
        const auto& s = v.get<std::string>();
        bool needs_quotes =
            s.empty() || s.find(':') != std::string::npos ||
            s.find('#') != std::string::npos ||
            (s.size() > 0 && (std::isspace(static_cast<unsigned char>(s.front())) ||
                              std::isspace(static_cast<unsigned char>(s.back()))));
        if (needs_quotes) {
            os << '"';
            for (char c : s) {
                if (c == '"' || c == '\\') os << '\\';
                os << c;
            }
            os << '"';
        } else {
            os << s;
        }
    } else if (v.is_array()) {
        if (v.empty()) { os << "[]"; return; }
        os << "\n";
        for (const auto& item : v) {
            os << pad(indent) << "- ";
            if (item.is_object() || item.is_array()) {
                std::ostringstream inner;
                dump_yaml(inner, item, indent + 2);
                auto s = inner.str();
                // If inner started with a newline, splice it in directly;
                // otherwise inline.
                if (!s.empty() && s.front() == '\n') {
                    os << s.substr(1) << "\n";
                } else {
                    os << s << "\n";
                }
            } else {
                dump_yaml(os, item, indent + 2);
                os << "\n";
            }
        }
    } else if (v.is_object()) {
        if (v.empty()) { os << "{}"; return; }
        os << "\n";
        for (auto it = v.begin(); it != v.end(); ++it) {
            os << pad(indent) << it.key() << ":";
            if (it.value().is_object() || it.value().is_array()) {
                std::ostringstream inner;
                dump_yaml(inner, it.value(), indent + 2);
                os << inner.str();
                // Object/array dumps always ended with a newline.
                // Avoid double newlines.
                auto s = os.str();
                if (s.size() >= 2 && s[s.size() - 1] == '\n' &&
                    s[s.size() - 2] == '\n') {
                    // Trim one.
                    os.str(s.substr(0, s.size() - 1));
                    os.seekp(0, std::ios::end);
                }
            } else {
                os << ' ';
                dump_yaml(os, it.value(), indent + 2);
                os << "\n";
            }
        }
    }
}

// Read a small text file fully (used by parse_env_file).
std::string read_text_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

// Trim ASCII whitespace from both ends.
std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

// Default list of configurable fields for environments whose config
// class we cannot introspect at runtime.  Mirrors the common subset of
// atroposlib.envs.base.BaseEnvConfig.
nlohmann::json default_env_config_fields() {
    nlohmann::json j = nlohmann::json::object();
    auto add = [&](const char* name, const char* type,
                   const nlohmann::json& dflt, const char* desc,
                   bool locked) {
        j[name] = {
            {"type", type},
            {"default", dflt},
            {"description", desc},
            {"locked", locked},
            {"current_value", dflt},
        };
    };
    add("group_size", "int", 16, "Rollouts per prompt group", false);
    add("use_wandb", "bool", true, "Stream metrics to Weights & Biases", true);
    add("rollout_server_url", "str", "http://localhost:8000",
        "Atropos trajectory server URL", true);
    add("tokenizer_name", "str", "Qwen/Qwen3-8B",
        "HuggingFace tokenizer for rollout workers", true);
    add("wandb_name", "str", "", "Custom WandB run name", false);
    add("wandb_project", "str", "atropos-tinker", "WandB project", false);
    add("max_token_length", "int", 8192, "Max tokens per rollout", true);
    add("max_num_workers", "int", 2048, "Parallel rollout worker limit", true);
    add("worker_timeout", "int", 3600, "Per-worker timeout (seconds)", true);
    add("total_steps", "int", 2500, "Trainer steps", true);
    add("steps_per_eval", "int", 25, "Train steps between evals", true);
    add("max_batches_offpolicy", "int", 3,
        "Max off-policy batches before refresh", true);
    add("inference_weight", "float", 1.0, "Rollout model sampling weight",
        true);
    add("eval_limit_ratio", "float", 0.1, "Eval set fraction", true);
    add("dataset_name", "str", "",
        "HuggingFace dataset slug (override per env)", false);
    add("dataset_split", "str", "train", "HuggingFace split", false);
    add("num_eval_examples", "int", 128, "Eval set size", false);
    add("reward_mean_target", "float", 0.0, "Optional reward mean target",
        false);
    add("temperature", "float", 1.0, "Rollout sampling temperature", false);
    add("top_p", "float", 1.0, "Nucleus sampling cutoff", false);
    return j;
}

}  // namespace

// ---------------------------------------------------------------------------
// locked_fields() — mirrors the Python LOCKED_FIELDS dict.
// ---------------------------------------------------------------------------
nlohmann::json locked_fields() {
    nlohmann::json j = {
        {"env", {
            {"tokenizer_name", "Qwen/Qwen3-8B"},
            {"rollout_server_url", "http://localhost:8000"},
            {"use_wandb", true},
            {"max_token_length", 8192},
            {"max_num_workers", 2048},
            {"worker_timeout", 3600},
            {"total_steps", 2500},
            {"steps_per_eval", 25},
            {"max_batches_offpolicy", 3},
            {"inference_weight", 1.0},
            {"eval_limit_ratio", 0.1},
        }},
        {"openai", nlohmann::json::array({
            {
                {"model_name", "Qwen/Qwen3-8B"},
                {"base_url", "http://localhost:8001/v1"},
                {"api_key", "x"},
                {"weight", 1.0},
                {"num_requests_for_eval", 256},
                {"timeout", 3600},
                {"server_type", "sglang"},
            },
        })},
        {"tinker", {
            {"lora_rank", 32},
            {"learning_rate", 0.00004},
            {"max_token_trainer_length", 9000},
            {"checkpoint_dir", "./temp/"},
            {"save_checkpoint_interval", 25},
        }},
        {"slurm", false},
        {"testing", false},
    };
    return j;
}

// ---------------------------------------------------------------------------
// parse_env_file — lenient text scan for BaseEnv subclasses.
// ---------------------------------------------------------------------------
std::optional<EnvironmentInfo> parse_env_file(const std::string& path) {
    auto src = read_text_file(path);
    if (src.empty()) return std::nullopt;

    std::istringstream is(src);
    std::string line;
    EnvironmentInfo info;
    info.file_path = path;
    {
        std::filesystem::path p(path);
        info.name = p.stem().string();
    }
    bool found_class = false;
    bool in_class_body = false;
    int class_indent = 0;
    bool docstring_captured = false;

    while (std::getline(is, line)) {
        // Locate `class Foo(BaseEnv):` (or Foo(X.BaseEnv):).
        if (!found_class) {
            auto pos = line.find("class ");
            if (pos != std::string::npos) {
                auto open = line.find('(', pos);
                auto colon = line.find("):", pos);
                if (open != std::string::npos && colon != std::string::npos &&
                    open < colon) {
                    auto bases = line.substr(open + 1, colon - open - 1);
                    bool inherits_base_env = false;
                    std::istringstream bs(bases);
                    std::string tok;
                    while (std::getline(bs, tok, ',')) {
                        tok = trim(tok);
                        // Strip leading `X.` if present.
                        auto dot = tok.rfind('.');
                        auto name = (dot == std::string::npos)
                                        ? tok
                                        : tok.substr(dot + 1);
                        if (name == "BaseEnv") {
                            inherits_base_env = true;
                            break;
                        }
                    }
                    if (inherits_base_env) {
                        info.class_name =
                            trim(line.substr(pos + 6, open - pos - 6));
                        class_indent =
                            static_cast<int>(line.find_first_not_of(" \t"));
                        if (class_indent < 0) class_indent = 0;
                        found_class = true;
                        in_class_body = true;
                        continue;
                    }
                }
            }
            continue;
        }

        if (!in_class_body) continue;

        // First indented line after the class — capture docstring.
        if (!docstring_captured) {
            auto stripped = trim(line);
            if (stripped.empty()) continue;
            if (stripped.rfind("\"\"\"", 0) == 0 ||
                stripped.rfind("'''", 0) == 0) {
                docstring_captured = true;
                auto quote = stripped.substr(0, 3);
                auto body = stripped.substr(3);
                auto end_pos = body.find(quote);
                std::string doc;
                if (end_pos != std::string::npos) {
                    doc = body.substr(0, end_pos);
                } else {
                    doc = body;
                    while (std::getline(is, line)) {
                        auto it = line.find(quote);
                        if (it != std::string::npos) {
                            doc += "\n" + line.substr(0, it);
                            break;
                        }
                        doc += "\n" + line;
                    }
                }
                auto nl = doc.find('\n');
                info.description = trim(nl == std::string::npos
                                             ? doc
                                             : doc.substr(0, nl));
                continue;
            }
        }

        // `name = "..."` or `env_config_cls = FooConfig`.
        auto stripped = line;
        // Stop at a dedent (next top-level definition).
        auto nonws = line.find_first_not_of(" \t");
        if (nonws != std::string::npos &&
            static_cast<int>(nonws) <= class_indent &&
            line.substr(nonws, 5) != "class") {
            break;
        }

        auto eq = stripped.find('=');
        if (eq != std::string::npos) {
            auto lhs = trim(stripped.substr(0, eq));
            auto rhs = trim(stripped.substr(eq + 1));
            if (lhs == "name" && rhs.size() >= 2 &&
                (rhs.front() == '"' || rhs.front() == '\'')) {
                auto q = rhs.front();
                auto end = rhs.find(q, 1);
                if (end != std::string::npos) {
                    info.name = rhs.substr(1, end - 1);
                }
            } else if (lhs == "env_config_cls") {
                // `env_config_cls = FooConfig`  — take the bare identifier.
                std::string cls;
                for (char c : rhs) {
                    if (std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '_') {
                        cls.push_back(c);
                    } else if (!cls.empty()) {
                        break;
                    }
                }
                if (!cls.empty()) info.config_class = cls;
            }
        }
    }

    if (!found_class) return std::nullopt;
    if (info.description.empty()) {
        info.description =
            "Environment from " +
            std::filesystem::path(path).filename().string();
    }
    return info;
}

// ---------------------------------------------------------------------------
// build_run_yaml — merge locked + user config, emit YAML.
// ---------------------------------------------------------------------------
std::string build_run_yaml(const std::string& env_name,
                           const nlohmann::json& user_config,
                           const std::string& wandb_project,
                           const std::string& wandb_run_name) {
    nlohmann::json merged = locked_fields();
    if (!merged.contains("env") || !merged["env"].is_object()) {
        merged["env"] = nlohmann::json::object();
    }
    if (user_config.is_object()) {
        for (auto it = user_config.begin(); it != user_config.end(); ++it) {
            if (it.value().is_null()) continue;
            if (it.value().is_string() && it.value().get<std::string>().empty()) {
                continue;
            }
            merged["env"][it.key()] = it.value();
        }
    }
    if (!merged["env"].contains("env_name")) {
        merged["env"]["env_name"] = env_name;
    }
    if (!merged.contains("tinker") || !merged["tinker"].is_object()) {
        merged["tinker"] = nlohmann::json::object();
    }
    merged["tinker"]["wandb_project"] = wandb_project;
    merged["tinker"]["wandb_run_name"] = wandb_run_name;

    std::ostringstream os;
    dump_yaml(os, merged, 0);
    auto s = os.str();
    if (!s.empty() && s.front() == '\n') s.erase(s.begin());
    return s;
}

// ---------------------------------------------------------------------------
// first_missing_env
// ---------------------------------------------------------------------------
std::string first_missing_env(const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        if (getenv_str(k.c_str()).empty()) return k;
    }
    return {};
}

// ---------------------------------------------------------------------------
// RlLocalRegistry
// ---------------------------------------------------------------------------

RlLocalRegistry& RlLocalRegistry::instance() {
    static RlLocalRegistry inst;
    return inst;
}

std::string RlLocalRegistry::tinker_root() const {
    return getenv_str("TINKER_ATROPOS_ROOT");
}

std::string RlLocalRegistry::environments_dir() const {
    auto root = tinker_root();
    if (root.empty()) return {};
    return (std::filesystem::path(root) / "tinker_atropos" / "environments")
        .string();
}

std::string RlLocalRegistry::configs_dir() const {
    auto root = tinker_root();
    if (root.empty()) return {};
    return (std::filesystem::path(root) / "configs").string();
}

std::string RlLocalRegistry::logs_dir() const {
    const char* home = std::getenv("HERMES_HOME");
    std::filesystem::path base;
    if (home && *home) {
        base = home;
    } else {
        const char* h = std::getenv("HOME");
        base = std::filesystem::path(h ? h : ".") / ".hermes";
    }
    return (base / "logs" / "rl_training").string();
}

std::vector<EnvironmentInfo> RlLocalRegistry::scan_environments() const {
    std::vector<EnvironmentInfo> out;
    auto dir = environments_dir();
    if (dir.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != ".py") continue;
        auto name = p.filename().string();
        if (!name.empty() && name.front() == '_') continue;
        auto info = parse_env_file(p.string());
        if (info) out.push_back(*info);
    }
    std::sort(out.begin(), out.end(),
              [](const EnvironmentInfo& a, const EnvironmentInfo& b) {
                  return a.name < b.name;
              });
    return out;
}

const std::vector<EnvironmentInfo>& RlLocalRegistry::environments() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!env_cache_ready_) {
        env_cache_ready_ = true;
        env_cache_ = scan_environments();
    }
    return env_cache_;
}

void RlLocalRegistry::invalidate_environment_cache() {
    std::lock_guard<std::mutex> lock(mu_);
    env_cache_ready_ = false;
    env_cache_.clear();
    env_fields_cache_.clear();
}

nlohmann::json RlLocalRegistry::env_config_fields(const std::string& env_name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = env_fields_cache_.find(env_name);
    if (it != env_fields_cache_.end()) return it->second;
    // Without a live Python interpreter we fall back to the known base
    // config fields; specific envs can only override defaults via user
    // config.
    auto fields = default_env_config_fields();
    env_fields_cache_[env_name] = fields;
    return fields;
}

std::optional<std::string> RlLocalRegistry::current_env() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_env_;
}

nlohmann::json RlLocalRegistry::current_config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_config_;
}

void RlLocalRegistry::set_current_env(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_env_ = name;
        current_config_ = nlohmann::json::object();
        // Auto-set wandb_name to "<env>-<timestamp>" to avoid collisions.
        current_config_["wandb_name"] =
            name + "-" + format_timestamp(std::chrono::system_clock::now());
    }
    // Pre-warm fields cache.
    (void)env_config_fields(name);
}

void RlLocalRegistry::set_current_config_field(const std::string& field,
                                               const nlohmann::json& value) {
    std::lock_guard<std::mutex> lock(mu_);
    current_config_[field] = value;
}

void RlLocalRegistry::reset_current_config() {
    std::lock_guard<std::mutex> lock(mu_);
    current_config_ = nlohmann::json::object();
}

RunState* RlLocalRegistry::create_run(const std::string& env_name,
                                      const nlohmann::json& config) {
    std::lock_guard<std::mutex> lock(mu_);
    auto id = random_run_id();
    while (active_runs_.count(id)) id = random_run_id();
    RunState r;
    r.run_id = id;
    r.environment = env_name;
    r.config = config;
    r.status = "starting";
    r.start_time = std::chrono::system_clock::now();
    r.wandb_project = config.value("wandb_project", "atropos-tinker");
    r.wandb_run_name = env_name + "-" + id;
    auto logs = logs_dir();
    r.api_log_path = logs + "/api_" + id + ".log";
    r.trainer_log_path = logs + "/trainer_" + id + ".log";
    r.env_log_path = logs + "/env_" + id + ".log";
    auto cfg_dir = configs_dir();
    if (!cfg_dir.empty()) {
        r.config_path = cfg_dir + "/run_" + id + ".yaml";
    }
    auto [it, _] = active_runs_.emplace(id, std::move(r));
    return &it->second;
}

RunState* RlLocalRegistry::get_run(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = active_runs_.find(run_id);
    if (it == active_runs_.end()) return nullptr;
    return &it->second;
}

std::vector<RunState> RlLocalRegistry::list_runs() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<RunState> out;
    out.reserve(active_runs_.size());
    for (const auto& kv : active_runs_) out.push_back(kv.second);
    return out;
}

void RlLocalRegistry::set_run_status(const std::string& run_id,
                                     const std::string& status,
                                     const std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = active_runs_.find(run_id);
    if (it == active_runs_.end()) return;
    it->second.status = status;
    if (!err.empty()) it->second.error_message = err;
}

int RlLocalRegistry::status_check_cooldown(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = last_check_.find(run_id);
    if (it == last_check_.end()) return 0;
    auto now = std::chrono::system_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - it->second)
            .count();
    if (elapsed >= kMinStatusCheckIntervalSeconds) return 0;
    return static_cast<int>(kMinStatusCheckIntervalSeconds - elapsed);
}

void RlLocalRegistry::record_status_check(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mu_);
    last_check_[run_id] = std::chrono::system_clock::now();
}

const std::vector<TestModel>& RlLocalRegistry::test_models() {
    static const std::vector<TestModel> models = {
        {"qwen/qwen3-8b", "Qwen3 8B", "small"},
        {"z-ai/glm-4.7-flash", "GLM-4.7 Flash", "medium"},
        {"minimax/minimax-m2.7", "MiniMax M2.7", "large"},
    };
    return models;
}

void RlLocalRegistry::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    env_cache_ready_ = false;
    env_cache_.clear();
    env_fields_cache_.clear();
    current_env_.reset();
    current_config_ = nlohmann::json::object();
    active_runs_.clear();
    last_check_.clear();
}

// ===========================================================================
// Handlers
// ===========================================================================

namespace {

// rl_list_environments ------------------------------------------------------
std::string handle_list_envs_local() {
    auto& reg = RlLocalRegistry::instance();
    nlohmann::json r;
    r["environments"] = nlohmann::json::array();
    for (const auto& env : reg.environments()) {
        r["environments"].push_back({
            {"name", env.name},
            {"class_name", env.class_name},
            {"file_path", env.file_path},
            {"description", env.description},
        });
    }
    r["count"] = r["environments"].size();
    r["tips"] = nlohmann::json::array(
        {"Use rl_select_environment(name) to select an environment",
         "Read the file_path with file tools to understand how each environment works",
         "Look for load_dataset(), score_answer(), get_next_item() methods"});
    return tool_result(r);
}

// rl_select_environment -----------------------------------------------------
std::string handle_select_env_local(const nlohmann::json& args) {
    auto name = args.at("name").get<std::string>();
    auto& reg = RlLocalRegistry::instance();
    const auto& envs = reg.environments();
    const EnvironmentInfo* found = nullptr;
    for (const auto& e : envs) {
        if (e.name == name) {
            found = &e;
            break;
        }
    }
    if (!found) {
        nlohmann::json available = nlohmann::json::array();
        for (const auto& e : envs) available.push_back(e.name);
        return tool_error("Environment '" + name + "' not found",
                          {{"available", available}});
    }
    reg.set_current_env(name);
    nlohmann::json r{
        {"message", "Selected environment: " + name},
        {"environment", name},
        {"file_path", found->file_path},
    };
    return tool_result(r);
}

// rl_get_current_config -----------------------------------------------------
std::string handle_get_config_local() {
    auto& reg = RlLocalRegistry::instance();
    auto cur = reg.current_env();
    if (!cur) {
        return tool_error(
            "No environment selected. Use rl_select_environment(name) first.");
    }
    auto fields = reg.env_config_fields(*cur);
    auto current = reg.current_config();
    nlohmann::json configurable = nlohmann::json::array();
    nlohmann::json locked = nlohmann::json::array();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        auto& v = it.value();
        nlohmann::json row{
            {"name", it.key()},
            {"type", v.value("type", "unknown")},
            {"default", v.value("default", nlohmann::json())},
            {"description", v.value("description", "")},
            {"current_value", current.value(
                                  it.key(),
                                  v.value("default", nlohmann::json()))},
        };
        if (v.value("locked", false)) {
            locked.push_back(row);
        } else {
            configurable.push_back(row);
        }
    }
    nlohmann::json r{
        {"environment", *cur},
        {"configurable_fields", configurable},
        {"locked_fields", locked},
        {"tip", "Use rl_edit_config(field, value) to change any configurable field."},
    };
    return tool_result(r);
}

// rl_edit_config ------------------------------------------------------------
std::string handle_edit_config_local(const nlohmann::json& args) {
    auto field = args.at("key").get<std::string>();
    auto value = args.at("value");
    auto& reg = RlLocalRegistry::instance();
    auto cur = reg.current_env();
    if (!cur) {
        return tool_error(
            "No environment selected. Use rl_select_environment(name) first.");
    }
    auto fields = reg.env_config_fields(*cur);
    if (!fields.contains(field)) {
        nlohmann::json avail = nlohmann::json::array();
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            avail.push_back(it.key());
        }
        return tool_error("Unknown field '" + field + "'",
                          {{"available_fields", avail}});
    }
    if (fields[field].value("locked", false)) {
        return tool_error(
            "Field '" + field + "' is locked and cannot be changed",
            {{"locked_value", locked_fields()["env"].value(
                                  field, nlohmann::json())}});
    }
    reg.set_current_config_field(field, value);
    nlohmann::json r{
        {"message", "Updated " + field},
        {"field", field},
        {"value", value},
        {"config", reg.current_config()},
    };
    return tool_result(r);
}

// rl_start_training ---------------------------------------------------------
std::string handle_start_training_local() {
    auto& reg = RlLocalRegistry::instance();
    auto cur = reg.current_env();
    if (!cur) {
        return tool_error(
            "No environment selected. Use rl_select_environment(name) first.");
    }
    auto missing = first_missing_env({"TINKER_API_KEY"});
    if (!missing.empty()) {
        return tool_error(missing + " not set. Add it to ~/.hermes/.env");
    }
    // Locate env file.
    const auto& envs = reg.environments();
    const EnvironmentInfo* info = nullptr;
    for (const auto& e : envs) {
        if (e.name == *cur) { info = &e; break; }
    }
    if (!info) {
        return tool_error("Environment file not found for '" + *cur + "'");
    }
    auto cfg_json = reg.current_config();
    auto* run = reg.create_run(*cur, cfg_json);

    // Write YAML config to disk.
    auto yaml = build_run_yaml(*cur, cfg_json, run->wandb_project,
                               run->wandb_run_name);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(run->config_path).parent_path(), ec);
    std::filesystem::create_directories(reg.logs_dir(), ec);
    {
        std::ofstream ofs(run->config_path);
        if (ofs) ofs << yaml;
    }

    reg.set_run_status(run->run_id, "starting");

    nlohmann::json r{
        {"run_id", run->run_id},
        {"status", "starting"},
        {"environment", *cur},
        {"config", cfg_json},
        {"wandb_project", run->wandb_project},
        {"wandb_run_name", run->wandb_run_name},
        {"config_path", run->config_path},
        {"logs", {
            {"api", run->api_log_path},
            {"trainer", run->trainer_log_path},
            {"env", run->env_log_path},
        }},
        {"message",
         "Training starting. Use rl_check_status(run_id) to monitor "
         "(recommended: every 30 minutes)."},
    };
    return tool_result(r);
}

// rl_check_status -----------------------------------------------------------
std::string handle_check_status_local(const nlohmann::json& args) {
    auto run_id = args.at("run_id").get<std::string>();
    auto& reg = RlLocalRegistry::instance();
    auto remaining = reg.status_check_cooldown(run_id);
    if (remaining > 0) {
        nlohmann::json r{
            {"rate_limited", true},
            {"run_id", run_id},
            {"message", std::string("Rate limited. Next check available in ") +
                            std::to_string(remaining / 60) + " minutes."},
            {"next_check_in_seconds", remaining},
        };
        return tool_result(r);
    }
    reg.record_status_check(run_id);
    auto* run = reg.get_run(run_id);
    if (!run) {
        nlohmann::json active = nlohmann::json::array();
        for (const auto& s : reg.list_runs()) active.push_back(s.run_id);
        return tool_error("Run '" + run_id + "' not found",
                          {{"active_runs", active}});
    }
    auto now = std::chrono::system_clock::now();
    auto mins = std::chrono::duration_cast<std::chrono::seconds>(
                    now - run->start_time).count() / 60.0;
    nlohmann::json r{
        {"run_id", run_id},
        {"status", run->status},
        {"environment", run->environment},
        {"running_time_minutes", mins},
        {"processes", {
            {"api", run->api_pid ? "running" : "exited"},
            {"trainer", run->trainer_pid ? "running" : "exited"},
            {"env", run->env_pid ? "running" : "exited"},
        }},
        {"wandb_project", run->wandb_project},
        {"wandb_run_name", run->wandb_run_name},
        {"logs", {
            {"api", run->api_log_path},
            {"trainer", run->trainer_log_path},
            {"env", run->env_log_path},
        }},
    };
    if (!run->error_message.empty()) r["error"] = run->error_message;
    return tool_result(r);
}

// rl_stop_training ----------------------------------------------------------
std::string handle_stop_training_local(const nlohmann::json& args) {
    auto run_id = args.at("run_id").get<std::string>();
    auto& reg = RlLocalRegistry::instance();
    auto* run = reg.get_run(run_id);
    if (!run) {
        nlohmann::json active = nlohmann::json::array();
        for (const auto& s : reg.list_runs()) active.push_back(s.run_id);
        return tool_error("Run '" + run_id + "' not found",
                          {{"active_runs", active}});
    }
    if (run->status != "running" && run->status != "starting") {
        nlohmann::json r{
            {"message", "Run '" + run_id +
                           "' is not running (status: " + run->status + ")"},
        };
        return tool_result(r);
    }
    reg.set_run_status(run_id, "stopped");
    nlohmann::json r{
        {"message", "Stopped training run '" + run_id + "'"},
        {"run_id", run_id},
        {"status", "stopped"},
    };
    return tool_result(r);
}

// rl_get_results ------------------------------------------------------------
std::string handle_get_results_local(const nlohmann::json& args) {
    auto run_id = args.at("run_id").get<std::string>();
    auto& reg = RlLocalRegistry::instance();
    auto* run = reg.get_run(run_id);
    if (!run) return tool_error("Run '" + run_id + "' not found");
    nlohmann::json r{
        {"run_id", run_id},
        {"status", run->status},
        {"environment", run->environment},
        {"wandb_project", run->wandb_project},
        {"wandb_run_name", run->wandb_run_name},
    };
    return tool_result(r);
}

// rl_list_runs --------------------------------------------------------------
std::string handle_list_runs_local() {
    auto& reg = RlLocalRegistry::instance();
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& s : reg.list_runs()) {
        runs.push_back({
            {"run_id", s.run_id},
            {"environment", s.environment},
            {"status", s.status},
            {"wandb_run_name", s.wandb_run_name},
        });
    }
    nlohmann::json r{
        {"runs", runs},
        {"count", runs.size()},
    };
    return tool_result(r);
}

// rl_test_inference ---------------------------------------------------------
std::string handle_test_inference_local(const nlohmann::json& args) {
    auto& reg = RlLocalRegistry::instance();
    auto cur = reg.current_env();
    if (!cur) {
        return tool_error(
            "No environment selected. Use rl_select_environment(name) first.");
    }
    auto missing = first_missing_env({"OPENROUTER_API_KEY"});
    if (!missing.empty()) {
        return tool_error(
            missing + " not set. Required for inference testing.");
    }
    int num_steps = args.value("num_steps", kDefaultNumSteps);
    int group_size = args.value("group_size", kDefaultGroupSize);
    std::vector<std::string> models;
    if (args.contains("models") && args["models"].is_array()) {
        for (const auto& m : args["models"]) {
            if (m.is_string()) models.push_back(m.get<std::string>());
        }
    } else {
        for (const auto& m : RlLocalRegistry::test_models()) {
            models.push_back(m.id);
        }
    }
    nlohmann::json model_list = nlohmann::json::array();
    for (const auto& id : models) model_list.push_back(id);
    nlohmann::json r{
        {"environment", *cur},
        {"num_steps", num_steps},
        {"group_size", group_size},
        {"models", model_list},
        {"message",
         "Inference test plan constructed. Run "
         "`python -m tinker_atropos.process` inside the tinker-atropos "
         "checkout to execute; the C++ tool surface does not embed "
         "Atropos directly."},
    };
    return tool_result(r);
}

}  // namespace

// ---------------------------------------------------------------------------
// register_rl_tools
// ---------------------------------------------------------------------------
void register_rl_tools(hermes::llm::HttpTransport* transport) {
    auto& reg = ToolRegistry::instance();
    reg.register_toolset_check("rl", rl_any_available);

    auto* tp = transport;
    const bool have_http = (tp != nullptr);

    auto http_or_local = [tp, have_http](auto http_handler, auto local_handler) {
        return [tp, have_http, http_handler, local_handler](
                   const nlohmann::json& args,
                   const ToolContext& /*ctx*/) -> std::string {
            if (have_http && rl_http_env_available()) {
                return http_handler(tp, args);
            }
            return local_handler(args);
        };
    };

    // 1. rl_list_environments
    {
        ToolEntry e;
        e.name = "rl_list_environments";
        e.toolset = "rl";
        e.description = "List available RL training environments";
        e.emoji = "\xF0\x9F\xA7\xAA";  // test tube
        e.check_fn = rl_any_available;
        e.schema = {{"type", "object"},
                    {"properties", nlohmann::json::object()}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json&) {
                return http_get(t, api_url() + "/environments");
            },
            [](const nlohmann::json&) { return handle_list_envs_local(); });
        reg.register_tool(std::move(e));
    }

    // 2. rl_select_environment
    {
        ToolEntry e;
        e.name = "rl_select_environment";
        e.toolset = "rl";
        e.description = "Select an RL training environment";
        e.emoji = "\xF0\x9F\x8E\xAF";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"name",
               {{"type", "string"},
                {"description", "Environment name"}}}}},
            {"required", nlohmann::json::array({"name"})}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json& args) {
                nlohmann::json body;
                body["name"] = args.at("name");
                return http_post(t, api_url() + "/environments/select", body);
            },
            handle_select_env_local);
        reg.register_tool(std::move(e));
    }

    // 3. rl_get_current_config
    {
        ToolEntry e;
        e.name = "rl_get_current_config";
        e.toolset = "rl";
        e.description = "Get current RL training configuration";
        e.emoji = "\xE2\x9A\x99";
        e.check_fn = rl_any_available;
        e.schema = {{"type", "object"},
                    {"properties", nlohmann::json::object()}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json&) {
                return http_get(t, api_url() + "/config");
            },
            [](const nlohmann::json&) { return handle_get_config_local(); });
        reg.register_tool(std::move(e));
    }

    // 4. rl_edit_config
    {
        ToolEntry e;
        e.name = "rl_edit_config";
        e.toolset = "rl";
        e.description = "Edit RL training configuration";
        e.emoji = "\xE2\x9C\x8F";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"key", {{"type", "string"}, {"description", "Config key"}}},
              {"value", {{"description", "New value"}}}}},
            {"required", nlohmann::json::array({"key", "value"})}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json& args) {
                nlohmann::json body;
                body["key"] = args.at("key");
                body["value"] = args.at("value");
                return http_patch(t, api_url() + "/config", body);
            },
            handle_edit_config_local);
        reg.register_tool(std::move(e));
    }

    // 5. rl_start_training
    {
        ToolEntry e;
        e.name = "rl_start_training";
        e.toolset = "rl";
        e.description = "Start an RL training run";
        e.emoji = "\xF0\x9F\x9A\x80";
        e.check_fn = rl_any_available;
        e.schema = {{"type", "object"},
                    {"properties", nlohmann::json::object()}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json&) {
                return http_post(t, api_url() + "/training/start",
                                 nlohmann::json::object());
            },
            [](const nlohmann::json&) { return handle_start_training_local(); });
        reg.register_tool(std::move(e));
    }

    // 6. rl_check_status
    {
        ToolEntry e;
        e.name = "rl_check_status";
        e.toolset = "rl";
        e.description = "Check status of an RL training run";
        e.emoji = "\xF0\x9F\x93\x8A";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id",
               {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json& args) {
                auto run_id = args.at("run_id").get<std::string>();
                return http_get(t, api_url() + "/training/" + run_id);
            },
            handle_check_status_local);
        reg.register_tool(std::move(e));
    }

    // 7. rl_stop_training
    {
        ToolEntry e;
        e.name = "rl_stop_training";
        e.toolset = "rl";
        e.description = "Stop an RL training run";
        e.emoji = "\xE2\x9B\x94";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id",
               {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json& args) {
                auto run_id = args.at("run_id").get<std::string>();
                return http_post(
                    t, api_url() + "/training/" + run_id + "/stop",
                    nlohmann::json::object());
            },
            handle_stop_training_local);
        reg.register_tool(std::move(e));
    }

    // 8. rl_get_results
    {
        ToolEntry e;
        e.name = "rl_get_results";
        e.toolset = "rl";
        e.description = "Get results of an RL training run";
        e.emoji = "\xF0\x9F\x93\x88";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id",
               {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json& args) {
                auto run_id = args.at("run_id").get<std::string>();
                return http_get(
                    t, api_url() + "/training/" + run_id + "/results");
            },
            handle_get_results_local);
        reg.register_tool(std::move(e));
    }

    // 9. rl_list_runs
    {
        ToolEntry e;
        e.name = "rl_list_runs";
        e.toolset = "rl";
        e.description = "List all RL training runs";
        e.emoji = "\xF0\x9F\x93\x8B";
        e.check_fn = rl_any_available;
        e.schema = {{"type", "object"},
                    {"properties", nlohmann::json::object()}};
        e.handler = http_or_local(
            [](hermes::llm::HttpTransport* t, const nlohmann::json&) {
                return http_get(t, api_url() + "/training/runs");
            },
            [](const nlohmann::json&) { return handle_list_runs_local(); });
        reg.register_tool(std::move(e));
    }

    // 10. score_trajectory — always HTTP (no local equivalent).
    {
        ToolEntry e;
        e.name = "score_trajectory";
        e.toolset = "rl";
        e.description =
            "Score a completed trajectory (conversations list) with the "
            "configured RL reward model; returns {score, breakdown}.";
        e.emoji = "\xF0\x9F\x8F\x85";
        e.check_fn = rl_http_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"conversations",
               {{"type", "array"},
                {"description",
                 "HuggingFace SFT conversations array (from/value pairs)"}}},
              {"reward_model",
               {{"type", "string"},
                {"description", "Optional reward model id"}}}}},
            {"required", nlohmann::json::array({"conversations"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext&) -> std::string {
            if (!tp) return tool_error("RL HTTP transport not configured");
            nlohmann::json body;
            body["conversations"] = args.at("conversations");
            if (args.contains("reward_model")) {
                body["reward_model"] = args["reward_model"];
            }
            return http_post(tp, api_url() + "/score", body);
        };
        reg.register_tool(std::move(e));
    }

    // 11. rl_test_inference
    {
        ToolEntry e;
        e.name = "rl_test_inference";
        e.toolset = "rl";
        e.description = "Test inference with a trained RL model";
        e.emoji = "\xF0\x9F\xA7\xAA";
        e.check_fn = rl_any_available;
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id",
               {{"type", "string"}, {"description", "Training run ID"}}},
              {"input",
               {{"type", "string"}, {"description", "Input for inference"}}},
              {"num_steps",
               {{"type", "integer"},
                {"description", "Steps to run (local mode)"}}},
              {"group_size",
               {{"type", "integer"},
                {"description", "Completions per step (local mode)"}}},
              {"models",
               {{"type", "array"},
                {"description", "Optional model IDs to test (local mode)"}}}}},
            {"required", nlohmann::json::array()}};
        e.handler = [tp, have_http](const nlohmann::json& args,
                                    const ToolContext&) -> std::string {
            if (have_http && rl_http_env_available() && args.contains("run_id")) {
                auto run_id = args.at("run_id").get<std::string>();
                nlohmann::json body;
                body["input"] = args.value("input", "");
                return http_post(
                    tp, api_url() + "/training/" + run_id + "/infer", body);
            }
            return handle_test_inference_local(args);
        };
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
