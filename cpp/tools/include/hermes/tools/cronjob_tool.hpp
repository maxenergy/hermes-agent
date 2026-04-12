// Cron job tool — create, list, run, pause, resume, delete scheduled jobs.
#pragma once

#include "hermes/tools/registry.hpp"

#include <chrono>
#include <string>

namespace hermes::tools {

struct CronJob {
    std::string id;
    std::string name;
    std::string schedule;    // cron expression (5 fields)
    std::string prompt;
    std::string model;
    bool paused = false;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_run;
};

// Validate a 5-field cron expression (minute hour dom month dow).
// Each field must be '*', '*/N', 'N', or 'N-M'.
bool parse_cron_expression(const std::string& expr);

void register_cronjob_tools(ToolRegistry& registry);

}  // namespace hermes::tools
