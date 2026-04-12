#include "hermes/tools/cronjob_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace hermes::tools {

using json = nlohmann::json;

namespace {

std::string generate_job_id() {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 15);
    const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i) id += hex[dis(gen)];
    return id;
}

// In-memory job store (Phase 8, no persistence).
struct JobStore {
    std::mutex mu;
    std::unordered_map<std::string, CronJob> jobs;
};

JobStore& job_store() {
    static JobStore store;
    return store;
}

}  // namespace

bool parse_cron_expression(const std::string& expr) {
    // Split into tokens.
    std::istringstream iss(expr);
    std::vector<std::string> fields;
    std::string tok;
    while (iss >> tok) fields.push_back(tok);
    if (fields.size() != 5) return false;

    // Each field must match: * | */N | N | N-M
    std::regex field_re(R"(\*|(\*/\d+)|(\d+)|(\d+-\d+))");
    for (const auto& f : fields) {
        if (!std::regex_match(f, field_re)) return false;
    }
    return true;
}

void register_cronjob_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "cronjob";
    e.toolset = "cron";
    e.description = "Manage scheduled cron jobs";
    e.emoji = "\xe2\x8f\xb0";  // alarm clock

    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["create", "list", "run", "pause", "resume", "delete"],
                "description": "Action to perform"
            },
            "name": {
                "type": "string",
                "description": "Job name (for create)"
            },
            "schedule": {
                "type": "string",
                "description": "Cron expression (5 fields, for create)"
            },
            "prompt": {
                "type": "string",
                "description": "Agent prompt to execute (for create)"
            },
            "model": {
                "type": "string",
                "description": "Optional model override"
            },
            "job_id": {
                "type": "string",
                "description": "Job ID (for run/pause/resume/delete)"
            }
        },
        "required": ["action"]
    })JSON");

    e.handler = [](const json& args, const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("action") || !args["action"].is_string()) {
            return tool_error("missing required parameter: action");
        }
        auto action = args["action"].get<std::string>();
        auto& store = job_store();

        if (action == "create") {
            auto name = args.value("name", "");
            auto schedule = args.value("schedule", "");
            auto prompt = args.value("prompt", "");
            if (name.empty()) return tool_error("missing required parameter: name");
            if (schedule.empty()) return tool_error("missing required parameter: schedule");
            if (prompt.empty()) return tool_error("missing required parameter: prompt");

            if (!parse_cron_expression(schedule)) {
                return tool_error("invalid cron expression: " + schedule);
            }

            CronJob job;
            job.id = generate_job_id();
            job.name = name;
            job.schedule = schedule;
            job.prompt = prompt;
            job.model = args.value("model", "");
            job.created_at = std::chrono::system_clock::now();

            std::lock_guard<std::mutex> lk(store.mu);
            store.jobs[job.id] = job;
            return tool_result({{"job_id", job.id}, {"created", true}});
        }

        if (action == "list") {
            std::lock_guard<std::mutex> lk(store.mu);
            json jobs_arr = json::array();
            for (const auto& [id, job] : store.jobs) {
                jobs_arr.push_back({
                    {"id", job.id},
                    {"name", job.name},
                    {"schedule", job.schedule},
                    {"prompt", job.prompt},
                    {"paused", job.paused}
                });
            }
            return tool_result({{"jobs", jobs_arr},
                                {"count", static_cast<int>(jobs_arr.size())}});
        }

        if (action == "run") {
            auto job_id = args.value("job_id", "");
            if (job_id.empty()) return tool_error("missing required parameter: job_id");
            std::lock_guard<std::mutex> lk(store.mu);
            auto it = store.jobs.find(job_id);
            if (it == store.jobs.end()) return tool_error("job not found: " + job_id);
            it->second.last_run = std::chrono::system_clock::now();
            return tool_result({{"triggered", true}, {"job_id", job_id}});
        }

        if (action == "pause") {
            auto job_id = args.value("job_id", "");
            if (job_id.empty()) return tool_error("missing required parameter: job_id");
            std::lock_guard<std::mutex> lk(store.mu);
            auto it = store.jobs.find(job_id);
            if (it == store.jobs.end()) return tool_error("job not found: " + job_id);
            it->second.paused = true;
            return tool_result({{"paused", true}, {"job_id", job_id}});
        }

        if (action == "resume") {
            auto job_id = args.value("job_id", "");
            if (job_id.empty()) return tool_error("missing required parameter: job_id");
            std::lock_guard<std::mutex> lk(store.mu);
            auto it = store.jobs.find(job_id);
            if (it == store.jobs.end()) return tool_error("job not found: " + job_id);
            it->second.paused = false;
            return tool_result({{"resumed", true}, {"job_id", job_id}});
        }

        if (action == "delete") {
            auto job_id = args.value("job_id", "");
            if (job_id.empty()) return tool_error("missing required parameter: job_id");
            std::lock_guard<std::mutex> lk(store.mu);
            auto it = store.jobs.find(job_id);
            if (it == store.jobs.end()) return tool_error("job not found: " + job_id);
            store.jobs.erase(it);
            return tool_result({{"deleted", true}, {"job_id", job_id}});
        }

        return tool_error("unknown action: " + action);
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
