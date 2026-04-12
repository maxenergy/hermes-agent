#include "hermes/skills/skill_commands.hpp"

#include "hermes/skills/skill_utils.hpp"
#include "hermes/core/path.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hermes::skills {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// load_skill_payload
// ---------------------------------------------------------------------------

std::optional<SkillPayload> load_skill_payload(std::string_view skill_name) {
    for (const auto& dir : get_all_skills_dirs()) {
        auto skill_dir = dir / std::string(skill_name);
        auto skill_md = skill_dir / "SKILL.md";

        std::error_code ec;
        if (!fs::is_regular_file(skill_md, ec)) continue;

        std::ifstream ifs(skill_md);
        if (!ifs) continue;

        std::string contents((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

        auto [meta, body] = parse_frontmatter(contents);

        SkillPayload payload;
        payload.name = std::string(skill_name);
        payload.content = std::move(body);
        payload.metadata = meta.is_null() ? nlohmann::json::object() : std::move(meta);
        return payload;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// build_plan_path
// ---------------------------------------------------------------------------

std::filesystem::path build_plan_path(std::string_view slug) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    // POSIX localtime_r
    localtime_r(&time_t_now, &tm_buf);

    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(4) << (tm_buf.tm_year + 1900)
       << std::setw(2) << (tm_buf.tm_mon + 1)
       << std::setw(2) << tm_buf.tm_mday
       << '-'
       << std::setw(2) << tm_buf.tm_hour
       << std::setw(2) << tm_buf.tm_min
       << std::setw(2) << tm_buf.tm_sec
       << '-' << slug << ".md";

    auto plans_dir = hermes::core::path::get_hermes_home() / "plans";
    return plans_dir / ss.str();
}

// ---------------------------------------------------------------------------
// Built-in skills
// ---------------------------------------------------------------------------

const std::vector<BuiltinSkill>& builtin_skills() {
    static const std::vector<BuiltinSkill> skills = {
        {"/plan",
         "Create a detailed implementation plan for the given task. "
         "Break it down into numbered steps. Identify files to modify, "
         "dependencies, and potential risks. Save the plan to a "
         ".hermes/plans/ file."},
        {"/debug",
         "Investigate the described bug or unexpected behavior. "
         "Systematically narrow down the root cause by examining "
         "logs, stack traces, recent changes, and relevant code paths. "
         "Propose a fix with minimal blast radius."},
        {"/web-research",
         "Search the web for up-to-date information about the given "
         "topic. Summarize findings with source URLs. Prefer official "
         "documentation and authoritative references."},
    };
    return skills;
}

}  // namespace hermes::skills
