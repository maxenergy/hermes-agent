#include <hermes/cron/jobs.hpp>
#include <hermes/core/atomic_io.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace hermes::cron {
namespace {

namespace fs = std::filesystem;

// Tiny UUIDv4 generator.
std::string make_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng);
    uint64_t b = dist(rng);
    // Force variant (10xxxxxx) and version (0100) bits.
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%012llx",
             static_cast<unsigned>(a >> 32),
             static_cast<unsigned>((a >> 16) & 0xFFFF),
             static_cast<unsigned>(a & 0xFFFF),
             static_cast<unsigned>(b >> 48),
             static_cast<unsigned long long>(b & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

int64_t to_epoch(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               tp.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point from_epoch(int64_t epoch) {
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(epoch));
}

// Minimal JSON serialization — we avoid pulling in nlohmann_json as a
// dependency for this module, keeping it header-light.  The format is
// simple enough that hand-rolled output + a tiny parser is practical.

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string job_to_json(const Job& j) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"id\": \"" << escape_json(j.id) << "\",\n";
    os << "  \"name\": \"" << escape_json(j.name) << "\",\n";
    os << "  \"schedule_str\": \"" << escape_json(j.schedule_str) << "\",\n";
    os << "  \"prompt\": \"" << escape_json(j.prompt) << "\",\n";
    os << "  \"model\": \"" << escape_json(j.model) << "\",\n";
    os << "  \"delivery_targets\": [";
    for (size_t i = 0; i < j.delivery_targets.size(); ++i) {
        if (i > 0) os << ", ";
        os << "\"" << escape_json(j.delivery_targets[i]) << "\"";
    }
    os << "],\n";
    os << "  \"paused\": " << (j.paused ? "true" : "false") << ",\n";
    os << "  \"max_retries\": " << j.max_retries << ",\n";
    os << "  \"created_at\": " << to_epoch(j.created_at) << ",\n";
    os << "  \"last_run\": " << to_epoch(j.last_run) << ",\n";
    os << "  \"next_run\": " << to_epoch(j.next_run) << ",\n";
    os << "  \"run_count\": " << j.run_count << ",\n";
    os << "  \"fail_count\": " << j.fail_count << "\n";
    os << "}";
    return os.str();
}

std::string result_to_json(const JobResult& r) {
    std::ostringstream os;
    os << "{";
    os << "\"job_id\":\"" << escape_json(r.job_id) << "\",";
    os << "\"run_id\":\"" << escape_json(r.run_id) << "\",";
    os << "\"output\":\"" << escape_json(r.output) << "\",";
    os << "\"success\":" << (r.success ? "true" : "false") << ",";
    os << "\"started_at\":" << to_epoch(r.started_at) << ",";
    os << "\"finished_at\":" << to_epoch(r.finished_at);
    os << "}";
    return os.str();
}

// Minimal JSON value extraction helpers (no dependency on a JSON library).
// These work on the simple JSON we produce ourselves.

std::string json_string(const std::string& json, const std::string& key) {
    auto needle = "\"" + key + "\": \"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        // Try without space after colon.
        needle = "\"" + key + "\":\"";
        pos = json.find(needle);
    }
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char next = json[i + 1];
            if (next == '"') { result += '"'; ++i; }
            else if (next == '\\') { result += '\\'; ++i; }
            else if (next == 'n') { result += '\n'; ++i; }
            else if (next == 'r') { result += '\r'; ++i; }
            else if (next == 't') { result += '\t'; ++i; }
            else { result += next; ++i; }
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

int64_t json_int(const std::string& json, const std::string& key) {
    auto needle = "\"" + key + "\": ";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        needle = "\"" + key + "\":";
        pos = json.find(needle);
    }
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    // Skip whitespace.
    while (pos < json.size() && json[pos] == ' ') ++pos;
    std::string num;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == ',' || json[i] == '\n' || json[i] == '}' ||
            json[i] == ' ') break;
        num += json[i];
    }
    if (num.empty()) return 0;
    return std::stoll(num);
}

bool json_bool(const std::string& json, const std::string& key) {
    auto needle = "\"" + key + "\": ";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        needle = "\"" + key + "\":";
        pos = json.find(needle);
    }
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return json.substr(pos, 4) == "true";
}

std::vector<std::string> json_string_array(const std::string& json,
                                            const std::string& key) {
    std::vector<std::string> result;
    auto needle = "\"" + key + "\": [";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        needle = "\"" + key + "\":[";
        pos = json.find(needle);
    }
    if (pos == std::string::npos) return result;
    pos += needle.size();
    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;
    auto arr = json.substr(pos, end - pos);
    // Extract quoted strings.
    size_t i = 0;
    while (i < arr.size()) {
        auto q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        // Find closing quote (handle escapes).
        std::string val;
        for (size_t j = q1 + 1; j < arr.size(); ++j) {
            if (arr[j] == '\\' && j + 1 < arr.size()) {
                val += arr[j + 1];
                ++j;
            } else if (arr[j] == '"') {
                i = j + 1;
                break;
            } else {
                val += arr[j];
            }
        }
        result.push_back(val);
    }
    return result;
}

Job json_to_job(const std::string& json) {
    Job j;
    j.id = json_string(json, "id");
    j.name = json_string(json, "name");
    j.schedule_str = json_string(json, "schedule_str");
    if (!j.schedule_str.empty()) {
        j.schedule = parse(j.schedule_str);
    }
    j.prompt = json_string(json, "prompt");
    j.model = json_string(json, "model");
    j.delivery_targets = json_string_array(json, "delivery_targets");
    j.paused = json_bool(json, "paused");
    j.max_retries = static_cast<int>(json_int(json, "max_retries"));
    j.created_at = from_epoch(json_int(json, "created_at"));
    j.last_run = from_epoch(json_int(json, "last_run"));
    j.next_run = from_epoch(json_int(json, "next_run"));
    j.run_count = static_cast<int>(json_int(json, "run_count"));
    j.fail_count = static_cast<int>(json_int(json, "fail_count"));
    return j;
}

JobResult json_to_result(const std::string& json) {
    JobResult r;
    r.job_id = json_string(json, "job_id");
    r.run_id = json_string(json, "run_id");
    r.output = json_string(json, "output");
    r.success = json_bool(json, "success");
    r.started_at = from_epoch(json_int(json, "started_at"));
    r.finished_at = from_epoch(json_int(json, "finished_at"));
    return r;
}

}  // namespace

JobStore::JobStore(fs::path store_dir) : dir_(std::move(store_dir)) {
    fs::create_directories(dir_ / "jobs");
    fs::create_directories(dir_ / "results");
}

fs::path JobStore::job_path(const std::string& id) const {
    return dir_ / "jobs" / (id + ".json");
}

fs::path JobStore::result_path(const std::string& job_id) const {
    return dir_ / "results" / (job_id + ".jsonl");
}

std::string JobStore::create(Job job) {
    if (job.id.empty()) {
        job.id = make_uuid();
    }
    auto json = job_to_json(job);
    hermes::core::atomic_io::atomic_write(job_path(job.id), json);
    return job.id;
}

std::optional<Job> JobStore::get(const std::string& id) {
    auto content = hermes::core::atomic_io::atomic_read(job_path(id));
    if (!content) return std::nullopt;
    return json_to_job(*content);
}

std::vector<Job> JobStore::list_all() {
    std::vector<Job> jobs;
    auto jobs_dir = dir_ / "jobs";
    if (!fs::exists(jobs_dir)) return jobs;
    for (const auto& entry : fs::directory_iterator(jobs_dir)) {
        if (entry.path().extension() == ".json") {
            auto content =
                hermes::core::atomic_io::atomic_read(entry.path());
            if (content) {
                jobs.push_back(json_to_job(*content));
            }
        }
    }
    return jobs;
}

void JobStore::update(const Job& job) {
    auto json = job_to_json(job);
    hermes::core::atomic_io::atomic_write(job_path(job.id), json);
}

void JobStore::remove(const std::string& id) {
    auto p = job_path(id);
    if (fs::exists(p)) {
        fs::remove(p);
    }
}

void JobStore::save_result(const JobResult& result) {
    auto p = result_path(result.job_id);
    auto line = result_to_json(result) + "\n";
    // Append to JSONL file.
    std::ofstream ofs(p, std::ios::app);
    ofs << line;
}

std::vector<JobResult> JobStore::get_results(const std::string& job_id,
                                              int limit) {
    auto p = result_path(job_id);
    auto content = hermes::core::atomic_io::atomic_read(p);
    if (!content) return {};
    std::vector<JobResult> results;
    std::istringstream iss(*content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            results.push_back(json_to_result(line));
        }
    }
    // Return last `limit` results.
    if (static_cast<int>(results.size()) > limit) {
        results.erase(results.begin(),
                      results.begin() +
                          (static_cast<int>(results.size()) - limit));
    }
    return results;
}

}  // namespace hermes::cron
