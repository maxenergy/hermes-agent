#include "hermes/cli/rl_commands.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include "hermes/batch/batch_runner.hpp"
#include "hermes/environments/local.hpp"
#include "hermes/tools/rl_training_tool.hpp"
#include "hermes/tools/terminal_tool.hpp"

namespace hermes::cli::rl {

namespace {

constexpr const char* kDefaultRlPrompt = R"(You are an automated post-training engineer specializing in reinforcement learning for language models.

## Your Capabilities

You have access to RL training tools for running reinforcement learning on models through Tinker-Atropos:

1. DISCOVER: Use rl_list_environments to see available RL environments.
2. INSPECT: Read environment files to understand verifiers, data loading, and rewards.
3. INSPECT DATA: Use the terminal to explore HuggingFace datasets.
4. CREATE: Copy existing environments as templates and modify for your needs.
5. CONFIGURE: Use rl_select_environment and rl_edit_config to set up training.
6. TEST: Always use rl_test_inference before full training to validate your setup.
7. TRAIN: Use rl_start_training to begin; rl_check_status to monitor (at least 30 minutes between checks).
8. EVALUATE: Use rl_get_results and analyze WandB metrics to assess performance.

## Guidelines

- Always test before training — training runs take hours.
- Monitor metrics (reward/mean, percent_correct).
- Wait at least 30 minutes between status checks on long tasks.
- Stop training early if metrics stagnate.
- Start with small total_steps to validate, then scale up.
)";

std::vector<std::string> eval_tools() {
    // Inference + read-only toolset for ``hermes rl eval``.
    return {"terminal", "web", "rl"};
}

std::vector<std::string> train_tools() {
    // Full RL toolset including training-mutation tools.
    return {"terminal", "web", "rl"};
}

// Dataset line → batch config task JSONL payload.  If the input is
// already a JSON object we leave it alone; otherwise we wrap it as
// ``{"prompt": <line>}``.
std::string wrap_line(const std::string& raw) {
    auto trimmed = raw;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) return trimmed;
    try {
        auto j = nlohmann::json::parse(trimmed);
        if (j.is_object()) return trimmed;
    } catch (...) {
        // fallthrough
    }
    nlohmann::json obj;
    obj["prompt"] = trimmed;
    return obj.dump();
}

}  // namespace

std::string default_rl_system_prompt() {
    return std::string(kDefaultRlPrompt);
}

std::vector<std::string> rl_toolset_names(bool eval_only) {
    return eval_only ? eval_tools() : train_tools();
}

RlConfig load_rl_config(const std::filesystem::path& path) {
    RlConfig cfg;
    cfg.system_prompt = default_rl_system_prompt();

    if (path.empty() || !std::filesystem::exists(path)) {
        return cfg;
    }

    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto apply_string = [&](const std::string& key, std::string& out,
                             const nlohmann::json& j) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    auto apply_int = [&](const std::string& key, int& out,
                          const nlohmann::json& j) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
    };
    auto apply_bool = [&](const std::string& key, bool& out,
                           const nlohmann::json& j) {
        if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };

    nlohmann::json root;
    try {
        if (ext == ".yaml" || ext == ".yml") {
            YAML::Node y = YAML::LoadFile(path.string());
            // Round-trip through a JSON-ish emitter.  yaml-cpp does not
            // expose direct JSON conversion, so we serialise value-by-value.
            std::function<nlohmann::json(const YAML::Node&)> cvt;
            cvt = [&](const YAML::Node& n) -> nlohmann::json {
                if (n.IsMap()) {
                    nlohmann::json o = nlohmann::json::object();
                    for (const auto& kv : n) {
                        o[kv.first.as<std::string>()] = cvt(kv.second);
                    }
                    return o;
                }
                if (n.IsSequence()) {
                    nlohmann::json a = nlohmann::json::array();
                    for (const auto& it : n) a.push_back(cvt(it));
                    return a;
                }
                if (n.IsScalar()) {
                    // Try int / bool / fall back to string.
                    try { return n.as<int>(); } catch (...) {}
                    try { return n.as<bool>(); } catch (...) {}
                    return n.as<std::string>();
                }
                return nullptr;
            };
            root = cvt(y);
        } else {
            std::ifstream in(path);
            root = nlohmann::json::parse(in);
        }
    } catch (const std::exception& e) {
        std::cerr << "hermes rl: failed to parse config " << path
                  << ": " << e.what() << "\n";
        return cfg;
    }

    std::string ds, od, model, base_url, env, prompt;
    apply_string("dataset", ds, root);
    apply_string("output_dir", od, root);
    apply_string("model", model, root);
    apply_string("base_url", base_url, root);
    apply_string("environment", env, root);
    apply_string("system_prompt", prompt, root);
    if (!ds.empty()) cfg.dataset_path = ds;
    if (!od.empty()) cfg.output_dir = od;
    if (!model.empty()) cfg.model = model;
    if (!base_url.empty()) cfg.base_url = base_url;
    if (!env.empty()) cfg.default_environment = env;
    if (!prompt.empty()) cfg.system_prompt = prompt;

    apply_int("max_iterations", cfg.max_iterations, root);
    apply_int("num_workers", cfg.num_workers, root);
    apply_int("progress_interval", cfg.progress_interval_seconds, root);
    apply_int("checkpoint_interval", cfg.checkpoint_interval_seconds, root);
    apply_bool("eval_only", cfg.eval_only, root);
    apply_bool("save_trajectories", cfg.save_trajectories, root);

    if (root.contains("toolsets") && root["toolsets"].is_array()) {
        cfg.enabled_toolsets.clear();
        for (const auto& t : root["toolsets"]) {
            if (t.is_string()) cfg.enabled_toolsets.push_back(t.get<std::string>());
        }
    }

    return cfg;
}

static int run_loop(const RlConfig& cfg, bool eval_only) {
    if (cfg.dataset_path.empty()) {
        std::cerr << "hermes rl: dataset_path required\n";
        return 2;
    }
    if (!std::filesystem::exists(cfg.dataset_path)) {
        std::cerr << "hermes rl: dataset file not found: "
                  << cfg.dataset_path << "\n";
        return 2;
    }
    std::filesystem::create_directories(
        cfg.output_dir.empty() ? std::filesystem::path("./rl_out")
                                : cfg.output_dir);

    // Rewrite dataset into the batch runner's expected shape by wrapping
    // loose prompt strings as ``{"prompt": ...}`` objects.  Write to a
    // tmp file in the output dir so the original dataset is unchanged.
    auto wrapped_path = cfg.output_dir / "rl_dataset.jsonl";
    {
        std::ifstream in(cfg.dataset_path);
        std::ofstream out(wrapped_path);
        std::string line;
        while (std::getline(in, line)) {
            auto w = wrap_line(line);
            if (!w.empty()) out << w << "\n";
        }
    }

    batch::BatchConfig bc;
    bc.dataset_path = wrapped_path;
    bc.output_dir = cfg.output_dir;
    bc.num_workers = std::max(1, cfg.num_workers);
    bc.model = cfg.model;
    bc.enabled_toolsets = eval_only ? eval_tools() : train_tools();
    bc.default_environment = cfg.default_environment;
    bc.default_task_type = "prompt";
    bc.save_trajectories = cfg.save_trajectories;
    bc.progress_interval =
        std::chrono::seconds(std::max(1, cfg.progress_interval_seconds));
    bc.checkpoint_interval =
        std::chrono::seconds(std::max(1, cfg.checkpoint_interval_seconds));

    // Install a terminal env factory so any ``terminal`` tool invoked
    // mid-task is routed through the per-task environment.  Non-local
    // backends currently fall back to LocalEnvironment — same caveat
    // as the batch runner's make_env().
    hermes::tools::set_terminal_env_factory(
        [](const std::string& /*env_name*/)
            -> std::unique_ptr<hermes::environments::BaseEnvironment> {
            return std::make_unique<hermes::environments::LocalEnvironment>();
        });

    batch::BatchRunner runner(std::move(bc));
    runner.set_progress_callback([](const batch::BatchProgress& p) {
        std::cerr << "hermes rl progress: " << p.to_json().dump() << "\n";
    });
    auto result = runner.run();

    // Clear factory on exit so subsequent CLI operations see the
    // default local terminal.
    hermes::tools::set_terminal_env_factory({});

    // Emit a final summary to stdout in JSON so CI can parse it.
    nlohmann::json summary;
    summary["mode"] = eval_only ? "eval" : "train";
    summary["total"] = result.total_prompts;
    summary["completed"] = result.completed;
    summary["failed"] = result.failed;
    summary["duration_ms"] =
        static_cast<std::int64_t>(result.duration.count());
    std::cout << summary.dump() << "\n";
    return result.failed == 0 ? 0 : 1;
}

int run_rl_training(const RlConfig& config) {
    return run_loop(config, /*eval_only=*/false);
}

int run_rl_eval(const RlConfig& config) {
    return run_loop(config, /*eval_only=*/true);
}

namespace {

void print_rl_help() {
    std::cout <<
        "usage: hermes rl <train|eval|list-environments> [options]\n"
        "\n"
        "  train   Run RL training loop with the full toolset.\n"
        "  eval    Run inference-only evaluation against a dataset.\n"
        "  list-environments  List available RL environments.\n"
        "\n"
        "Common options:\n"
        "  --config <file>        Load RlConfig from YAML/JSON file.\n"
        "  --dataset <file>       JSONL dataset (one prompt per line).\n"
        "  --output-dir <dir>     Where to write trajectories + checkpoints.\n"
        "  --model <id>           Model id (default: claude-opus-4.5).\n"
        "  --environment <name>   local|docker|modal (default: local).\n"
        "  --max-iterations <n>   Agent iteration cap (default: 200).\n"
        "  --workers <n>          Parallel workers (default: 1).\n";
}

// Very small argv parser — enough for the rl subcommand surface.
struct ParsedArgs {
    std::string verb;
    std::filesystem::path config_path;
    std::string dataset;
    std::string output_dir;
    std::string model;
    std::string environment;
    int max_iterations = 0;
    int workers = 0;
    bool help = false;
    bool ok = true;
    std::string error;
};

ParsedArgs parse_args(const std::vector<std::string>& argv) {
    ParsedArgs a;
    if (argv.empty()) {
        a.help = true;
        return a;
    }
    a.verb = argv[0];
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const auto& k = argv[i];
        auto next = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argv.size()) {
                a.ok = false;
                a.error = flag + " requires a value";
                return {};
            }
            return argv[++i];
        };
        if (k == "--help" || k == "-h") a.help = true;
        else if (k == "--config") a.config_path = next(k);
        else if (k == "--dataset") a.dataset = next(k);
        else if (k == "--output-dir") a.output_dir = next(k);
        else if (k == "--model") a.model = next(k);
        else if (k == "--environment") a.environment = next(k);
        else if (k == "--max-iterations") {
            try { a.max_iterations = std::stoi(next(k)); }
            catch (...) { a.ok = false; a.error = "invalid --max-iterations"; }
        }
        else if (k == "--workers") {
            try { a.workers = std::stoi(next(k)); }
            catch (...) { a.ok = false; a.error = "invalid --workers"; }
        } else {
            a.ok = false;
            a.error = "unknown argument: " + k;
        }
        if (!a.ok) break;
    }
    return a;
}

}  // namespace

int dispatch_rl_command(const std::vector<std::string>& argv) {
    auto args = parse_args(argv);
    if (!args.ok) {
        std::cerr << "hermes rl: " << args.error << "\n";
        print_rl_help();
        return 2;
    }
    if (args.help || args.verb == "--help" || args.verb == "-h") {
        print_rl_help();
        return 0;
    }

    if (args.verb == "list-environments") {
        auto& reg = hermes::tools::RlLocalRegistry::instance();
        const auto& envs = reg.environments();
        if (envs.empty()) {
            std::cout << "No RL environments found.\n"
                      << "  - Set $TINKER_ATROPOS_ROOT to the tinker-atropos "
                         "checkout, or\n"
                      << "  - Drop environment modules under "
                      << reg.environments_dir() << "\n";
            return 0;
        }
        std::cout << "Available RL environments (" << envs.size() << "):\n";
        for (const auto& e : envs) {
            std::cout << "  - " << e.name;
            if (!e.class_name.empty()) std::cout << " (" << e.class_name << ")";
            if (!e.description.empty()) {
                auto desc = e.description;
                if (desc.size() > 80) desc.resize(77), desc.append("...");
                std::cout << " — " << desc;
            }
            std::cout << "\n";
            if (!e.file_path.empty()) {
                std::cout << "      " << e.file_path << "\n";
            }
        }
        return 0;
    }

    if (args.verb != "train" && args.verb != "eval") {
        std::cerr << "hermes rl: unknown verb '" << args.verb << "'\n";
        print_rl_help();
        return 2;
    }

    RlConfig cfg;
    if (!args.config_path.empty()) {
        cfg = load_rl_config(args.config_path);
    } else {
        cfg.system_prompt = default_rl_system_prompt();
    }
    if (!args.dataset.empty()) cfg.dataset_path = args.dataset;
    if (!args.output_dir.empty()) cfg.output_dir = args.output_dir;
    if (!args.model.empty()) cfg.model = args.model;
    if (!args.environment.empty()) cfg.default_environment = args.environment;
    if (args.max_iterations > 0) cfg.max_iterations = args.max_iterations;
    if (args.workers > 0) cfg.num_workers = args.workers;
    cfg.eval_only = (args.verb == "eval");
    if (cfg.output_dir.empty()) {
        cfg.output_dir = std::filesystem::path("rl_out");
    }

    return cfg.eval_only ? run_rl_eval(cfg) : run_rl_training(cfg);
}

}  // namespace hermes::cli::rl
