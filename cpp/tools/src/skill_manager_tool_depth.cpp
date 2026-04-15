// Implementation of hermes/tools/skill_manager_tool_depth.hpp.
#include "hermes/tools/skill_manager_tool_depth.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace hermes::tools::skill_manager::depth {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::vector<std::string> split_segments(std::string_view path) {
    std::vector<std::string> out{};
    std::string acc{};
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!acc.empty()) out.push_back(acc);
            acc.clear();
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty()) out.push_back(acc);
    return out;
}

const std::unordered_set<std::string>& allowed_subdirs() {
    static const std::unordered_set<std::string> k{
        "references", "templates", "scripts", "assets",
    };
    return k;
}

std::string format_with_commas(std::size_t n) {
    std::string digits{std::to_string(n)};
    std::string out{};
    int count{0};
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count != 0 && count % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

// ---- Action routing -----------------------------------------------------

Action parse_action(std::string_view raw) {
    std::string t{to_lower(trim_ascii(raw))};
    if (t == "list") return Action::List;
    if (t == "search") return Action::Search;
    if (t == "install") return Action::Install;
    if (t == "uninstall") return Action::Uninstall;
    if (t == "update") return Action::Update;
    if (t == "create") return Action::Create;
    if (t == "edit") return Action::Edit;
    if (t == "patch") return Action::Patch;
    if (t == "delete") return Action::Delete;
    if (t == "write_file") return Action::WriteFile;
    if (t == "remove_file") return Action::RemoveFile;
    if (t == "view") return Action::View;
    return Action::Unknown;
}

std::string action_name(Action action) {
    switch (action) {
        case Action::List: return "list";
        case Action::Search: return "search";
        case Action::Install: return "install";
        case Action::Uninstall: return "uninstall";
        case Action::Update: return "update";
        case Action::Create: return "create";
        case Action::Edit: return "edit";
        case Action::Patch: return "patch";
        case Action::Delete: return "delete";
        case Action::WriteFile: return "write_file";
        case Action::RemoveFile: return "remove_file";
        case Action::View: return "view";
        case Action::Unknown:
        default: return "unknown";
    }
}

bool action_requires_name(Action action) {
    switch (action) {
        case Action::Uninstall:
        case Action::Update:
        case Action::Create:
        case Action::Edit:
        case Action::Patch:
        case Action::Delete:
        case Action::WriteFile:
        case Action::RemoveFile:
        case Action::View:
            return true;
        default:
            return false;
    }
}

bool action_requires_content(Action action) {
    switch (action) {
        case Action::Create:
        case Action::Edit:
            return true;
        default:
            return false;
    }
}

// ---- Content-size validation -------------------------------------------

std::string validate_content_size(std::size_t chars, std::string_view label) {
    if (chars <= kMaxSkillContentChars) {
        return {};
    }
    std::ostringstream os;
    os << label << " content is " << format_with_commas(chars)
       << " characters (limit: "
       << format_with_commas(kMaxSkillContentChars)
       << "). Consider splitting into a smaller SKILL.md with supporting "
          "files in references/ or templates/.";
    return os.str();
}

std::string validate_file_bytes(std::size_t bytes, std::string_view label) {
    if (bytes <= kMaxSkillFileBytes) {
        return {};
    }
    std::ostringstream os;
    os << label << " is " << format_with_commas(bytes)
       << " bytes (limit: " << format_with_commas(kMaxSkillFileBytes)
       << "). Supporting files must stay under 1 MiB.";
    return os.str();
}

// ---- Frontmatter structural check --------------------------------------

FrontmatterCheck check_frontmatter_structure(std::string_view content) {
    FrontmatterCheck out{};
    std::string_view c{content};
    // Strip leading whitespace for the non-empty check.
    std::string trimmed{trim_ascii(c)};
    if (trimmed.empty()) {
        out.error = "Content cannot be empty.";
        return out;
    }
    // Require the original content to start with "---" (no leading
    // whitespace is tolerated — matches Python's
    // ``content.startswith("---")``).
    if (c.size() < 3 || c.substr(0, 3) != "---") {
        out.error =
            "SKILL.md must start with YAML frontmatter (---). See existing "
            "skills for format.";
        return out;
    }
    // Search for a closing "\n---" after the opener.
    std::string body{c.substr(3)};
    auto end = body.find("\n---");
    if (end == std::string::npos) {
        out.error =
            "SKILL.md frontmatter is not closed. Ensure you have a closing "
            "'---' line.";
        return out;
    }
    out.yaml_block = body.substr(0, end);
    auto after = end + 4;  // past "\n---"
    // Skip to end-of-line.
    while (after < body.size() &&
           (body[after] == ' ' || body[after] == '\t')) {
        ++after;
    }
    if (after < body.size() && body[after] == '\r') ++after;
    if (after < body.size() && body[after] == '\n') ++after;
    out.body = body.substr(after);
    // Require a non-empty body.
    if (trim_ascii(out.body).empty()) {
        out.error =
            "SKILL.md must have content after the frontmatter (instructions, "
            "procedures, etc.).";
        return out;
    }
    out.ok = true;
    return out;
}

std::string validate_frontmatter_keys(const nlohmann::json& parsed) {
    if (!parsed.is_object()) {
        return "Frontmatter must be a YAML mapping (key: value pairs).";
    }
    if (!parsed.contains("name")) {
        return "Frontmatter must include 'name' field.";
    }
    if (!parsed.contains("description")) {
        return "Frontmatter must include 'description' field.";
    }
    const auto& d = parsed.at("description");
    std::string desc{};
    if (d.is_string()) {
        desc = d.get<std::string>();
    } else {
        desc = d.dump();
    }
    constexpr std::size_t kMax{1024u};
    if (desc.size() > kMax) {
        std::ostringstream os;
        os << "Description exceeds " << kMax << " characters.";
        return os.str();
    }
    return {};
}

// ---- Relative-path resolution ------------------------------------------

NormalisedRelPath normalise_relative_path(std::string_view rel) {
    NormalisedRelPath out{};
    if (rel.empty()) {
        out.error = "file_path is required.";
        return out;
    }
    // Reject absolute paths.
    if (rel.front() == '/' || rel.front() == '\\') {
        out.error = "File path must be relative, not absolute.";
        return out;
    }
    // Reject Windows drive-letter paths.
    if (rel.size() >= 2 && rel[1] == ':') {
        out.error = "File path must be relative, not absolute.";
        return out;
    }
    std::vector<std::string> segs{split_segments(rel)};
    std::vector<std::string> cleaned{};
    for (const auto& seg : segs) {
        if (seg == ".") continue;
        if (seg == "..") {
            out.error = "Path traversal ('..') is not allowed.";
            return out;
        }
        cleaned.push_back(seg);
    }
    if (cleaned.empty()) {
        out.error = "File path must contain at least one segment.";
        return out;
    }
    if (allowed_subdirs().count(cleaned.front()) == 0u) {
        std::vector<std::string> sorted{allowed_subdirs().begin(),
                                         allowed_subdirs().end()};
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream os;
        os << "File must be under one of: ";
        for (std::size_t i{0}; i < sorted.size(); ++i) {
            if (i != 0u) os << ", ";
            os << sorted[i];
        }
        os << ". Got: '" << rel << "'";
        out.error = os.str();
        return out;
    }
    if (cleaned.size() < 2u) {
        std::ostringstream os;
        os << "Provide a file path, not just a directory. Example: '"
           << cleaned.front() << "/myfile.md'";
        out.error = os.str();
        return out;
    }
    std::ostringstream os;
    for (std::size_t i{0}; i < cleaned.size(); ++i) {
        if (i != 0u) os << '/';
        os << cleaned[i];
    }
    out.cleaned = os.str();
    return out;
}

bool first_segment_is_allowed(std::string_view rel) {
    std::vector<std::string> segs{split_segments(rel)};
    if (segs.empty()) return false;
    return allowed_subdirs().count(segs.front()) != 0u;
}

std::string atomic_temp_name(std::string_view target_name, unsigned seed) {
    // Match the Python prefix `.<name>.tmp.<suffix>` shape.  The suffix
    // is hex of ``seed`` so tests can assert exact output.
    std::ostringstream os;
    os << '.' << target_name << ".tmp." << std::hex << seed;
    return os.str();
}

// ---- Response payloads --------------------------------------------------

nlohmann::json error_payload(std::string_view message) {
    nlohmann::json out;
    out["success"] = false;
    out["error"] = std::string{message};
    return out;
}

nlohmann::json success_message_payload(std::string_view message) {
    nlohmann::json out;
    out["success"] = true;
    out["message"] = std::string{message};
    return out;
}

nlohmann::json not_found_payload(std::string_view skill_name) {
    std::ostringstream os;
    os << "Skill '" << skill_name << "' not found.";
    return error_payload(os.str());
}

nlohmann::json structural_break_payload(std::string_view underlying) {
    std::ostringstream os;
    os << "Patch would break SKILL.md structure: " << underlying;
    return error_payload(os.str());
}

// ---- Search / filter ---------------------------------------------------

std::vector<MinimalSkill> substring_search(
    const std::vector<MinimalSkill>& skills, std::string_view query) {
    std::string q{to_lower(trim_ascii(query))};
    if (q.empty()) return skills;
    std::vector<MinimalSkill> out{};
    for (const auto& s : skills) {
        std::string haystack{to_lower(s.name + " " + s.description)};
        if (haystack.find(q) != std::string::npos) {
            out.push_back(s);
        }
    }
    return out;
}

// ---- Patch-mode replacement --------------------------------------------

PatchResult exact_replace(std::string_view content, std::string_view old_s,
                          std::string_view new_s, bool replace_all) {
    PatchResult out{};
    if (old_s.empty()) {
        out.error = "old_string is required for 'patch'.";
        return out;
    }
    std::string src{content};
    std::string needle{old_s};
    std::string repl{new_s};

    // Count occurrences.
    std::size_t count{0};
    std::size_t pos{0};
    while (true) {
        auto hit = src.find(needle, pos);
        if (hit == std::string::npos) break;
        ++count;
        pos = hit + needle.size();
    }
    if (count == 0u) {
        out.error = "old_string was not found in the target file.";
        return out;
    }
    if (count > 1u && !replace_all) {
        std::ostringstream os;
        os << "old_string matches " << count
           << " times — pass replace_all=true or provide a more specific "
              "context.";
        out.error = os.str();
        return out;
    }
    // Apply.
    std::string work{src};
    std::size_t search{0};
    std::size_t applied{0};
    while (true) {
        auto hit = work.find(needle, search);
        if (hit == std::string::npos) break;
        work.replace(hit, needle.size(), repl);
        ++applied;
        search = hit + repl.size();
        if (!replace_all) break;
    }
    out.output = std::move(work);
    out.replacements = applied;
    return out;
}

}  // namespace hermes::tools::skill_manager::depth
