#include "hermes/state/process_registry.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hermes::state {

namespace {

constexpr int kWatchMaxPerWindow = 8;
constexpr auto kWatchWindow = std::chrono::seconds(10);
constexpr auto kWatchOverloadKill = std::chrono::seconds(45);

std::string make_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t v = dist(rng);
    std::ostringstream oss;
    oss << "proc_";
    for (int i = 0; i < 12; ++i) {
        int nib = static_cast<int>(v & 0xf);
        v >>= 4;
        oss << static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    return oss.str();
}

std::int64_t tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point ms_to_tp(std::int64_t ms) {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{ms}};
}

const char* state_to_string(ProcessState s) {
    switch (s) {
        case ProcessState::Running: return "running";
        case ProcessState::Exited: return "exited";
        case ProcessState::Killed: return "killed";
    }
    return "running";
}

ProcessState state_from_string(std::string_view s) {
    if (s == "exited") return ProcessState::Exited;
    if (s == "killed") return ProcessState::Killed;
    return ProcessState::Running;
}

nlohmann::json serialize_session(const ProcessSession& s) {
    nlohmann::json j;
    j["id"] = s.id;
    j["command"] = s.command;
    j["task_id"] = s.task_id;
    j["session_key"] = s.session_key;
    if (s.pid) j["pid"] = *s.pid;
    if (s.exit_code) j["exit_code"] = *s.exit_code;
    j["cwd"] = s.cwd.string();
    j["started_at"] = tp_to_ms(s.started_at);
    j["updated_at"] = tp_to_ms(s.updated_at);
    j["ended_at"] = tp_to_ms(s.ended_at);
    j["state"] = state_to_string(s.state);
    j["pid_scope"] = s.pid_scope == PidScope::Sandbox ? "sandbox" : "host";
    j["detached"] = s.detached;
    j["notify_on_complete"] = s.notify_on_complete;
    j["watch_patterns"] = s.watch_patterns;
    j["output_buffer"] = s.output_buffer;
    j["output_buffer_max"] = s.output_buffer_max;
    return j;
}

ProcessSession deserialize_session(const nlohmann::json& j) {
    ProcessSession s;
    s.id = j.value("id", std::string{});
    s.command = j.value("command", std::string{});
    s.task_id = j.value("task_id", std::string{});
    s.session_key = j.value("session_key", std::string{});
    if (j.contains("pid") && !j["pid"].is_null())
        s.pid = j["pid"].get<int>();
    if (j.contains("exit_code") && !j["exit_code"].is_null())
        s.exit_code = j["exit_code"].get<int>();
    s.cwd = j.value("cwd", std::string{});
    s.started_at = ms_to_tp(j.value("started_at", std::int64_t{0}));
    s.updated_at = ms_to_tp(j.value("updated_at", std::int64_t{0}));
    s.ended_at = ms_to_tp(j.value("ended_at", std::int64_t{0}));
    s.state = state_from_string(j.value("state", std::string{"running"}));
    s.pid_scope = (j.value("pid_scope", std::string{"host"}) == "sandbox")
                      ? PidScope::Sandbox
                      : PidScope::Host;
    s.detached = j.value("detached", false);
    s.notify_on_complete = j.value("notify_on_complete", false);
    if (j.contains("watch_patterns"))
        s.watch_patterns =
            j["watch_patterns"].get<std::vector<std::string>>();
    s.output_buffer = j.value("output_buffer", std::string{});
    s.output_buffer_max =
        j.value("output_buffer_max", static_cast<std::size_t>(200 * 1024));
    return s;
}

}  // namespace

// ---- ProcessSession inline members --------------------------------------

void ProcessSession::append_output(std::string_view chunk) {
    output_buffer.append(chunk.data(), chunk.size());
    if (output_buffer.size() > output_buffer_max) {
        // Preserve the tail — drop bytes from the front.
        std::size_t drop = output_buffer.size() - output_buffer_max;
        output_buffer.erase(0, drop);
    }
}

bool ProcessSession::overloaded() const {
    if (overload_start.time_since_epoch().count() == 0) return false;
    return (std::chrono::system_clock::now() - overload_start) >
           kWatchOverloadKill;
}

// ---- Impl ---------------------------------------------------------------

struct ProcessRegistry::Impl {
    std::filesystem::path checkpoint_path;
    mutable std::mutex mtx;
    std::unordered_map<std::string, ProcessSession> running;
    std::unordered_map<std::string, ProcessSession> finished;
    std::vector<ProcessSession> orphaned;
    std::deque<WatchNotification> notifications;

    explicit Impl(std::filesystem::path p) : checkpoint_path(std::move(p)) {}

    // Core watch-pattern scanner. Called from feed_output while the
    // outer mutex is held, so no additional locking needed here.
    void scan_chunk(ProcessSession& s, std::string_view chunk) {
        if (s.watch_patterns.empty()) return;

        auto now = std::chrono::system_clock::now();

        // Reset window if elapsed.
        if (s.window_start.time_since_epoch().count() == 0 ||
            (now - s.window_start) >= kWatchWindow) {
            s.window_start = now;
            s.notifications_in_window = 0;
        }

        // Scan chunk line by line.
        std::size_t pos = 0;
        while (pos <= chunk.size()) {
            std::size_t nl = chunk.find('\n', pos);
            std::string_view line =
                (nl == std::string_view::npos)
                    ? chunk.substr(pos)
                    : chunk.substr(pos, nl - pos);
            bool matched = false;
            std::string matched_pat;
            for (const auto& pat : s.watch_patterns) {
                if (!pat.empty() &&
                    line.find(pat) != std::string_view::npos) {
                    matched = true;
                    matched_pat = pat;
                    break;
                }
            }

            if (matched) {
                if (s.notifications_in_window < kWatchMaxPerWindow) {
                    WatchNotification n;
                    n.process_id = s.id;
                    n.pattern = matched_pat;
                    n.line = std::string(line);
                    n.at = now;
                    notifications.push_back(std::move(n));
                    ++s.notifications_in_window;
                    // A successful delivery clears the overload clock.
                    s.overload_start = {};
                } else {
                    // Rate-limited — start (or keep) the overload clock.
                    if (s.overload_start.time_since_epoch().count() == 0) {
                        s.overload_start = now;
                    } else if ((now - s.overload_start) >
                               kWatchOverloadKill) {
                        // Sustained overload → kill and emit a synthetic
                        // notification. We flip the state here but defer
                        // cleanup to the caller (move_to_finished happens
                        // below so we don't invalidate the reference).
                        s.state = ProcessState::Killed;
                        s.ended_at = now;
                        WatchNotification n;
                        n.process_id = s.id;
                        n.pattern = matched_pat;
                        n.line =
                            "watch_pattern overload - process killed";
                        n.at = now;
                        n.synthetic = true;
                        notifications.push_back(std::move(n));
                        return;
                    }
                }
            }

            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
    }
};

// ---- Construction / destruction -----------------------------------------

ProcessRegistry::ProcessRegistry()
    : ProcessRegistry(hermes::core::path::get_hermes_home() /
                      "processes.json") {}

ProcessRegistry::ProcessRegistry(const std::filesystem::path& checkpoint_path)
    : impl_(std::make_unique<Impl>(checkpoint_path)) {}

ProcessRegistry::~ProcessRegistry() = default;

// ---- API ----------------------------------------------------------------

std::string ProcessRegistry::register_process(ProcessSession session) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (session.id.empty()) {
        session.id = make_id();
    }
    if (session.started_at.time_since_epoch().count() == 0) {
        session.started_at = std::chrono::system_clock::now();
    }
    session.updated_at = std::chrono::system_clock::now();
    std::string id = session.id;
    impl_->running[id] = std::move(session);
    return id;
}

std::optional<ProcessSession> ProcessRegistry::get(
    const std::string& id) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (auto it = impl_->running.find(id); it != impl_->running.end())
        return it->second;
    if (auto it = impl_->finished.find(id); it != impl_->finished.end())
        return it->second;
    return std::nullopt;
}

std::vector<ProcessSession> ProcessRegistry::list_running() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::vector<ProcessSession> out;
    out.reserve(impl_->running.size());
    for (const auto& [_, s] : impl_->running) out.push_back(s);
    return out;
}

std::vector<ProcessSession> ProcessRegistry::list_finished() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::vector<ProcessSession> out;
    out.reserve(impl_->finished.size());
    for (const auto& [_, s] : impl_->finished) out.push_back(s);
    return out;
}

void ProcessRegistry::mark_exited(const std::string& id, int exit_code) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    auto it = impl_->running.find(id);
    if (it == impl_->running.end()) return;
    ProcessSession s = std::move(it->second);
    impl_->running.erase(it);
    s.state = ProcessState::Exited;
    s.exit_code = exit_code;
    s.ended_at = std::chrono::system_clock::now();
    s.updated_at = s.ended_at;
    impl_->finished[id] = std::move(s);
}

void ProcessRegistry::kill(const std::string& id) {
    std::unique_lock<std::mutex> lk(impl_->mtx);
    auto it = impl_->running.find(id);
    if (it == impl_->running.end()) return;
    ProcessSession s = std::move(it->second);
    impl_->running.erase(it);
#ifndef _WIN32
    // Send SIGTERM, wait 2s, then SIGKILL if still alive.
    if (s.pid) {
        pid_t pid = *s.pid;
        // Unlock while waiting so other operations can proceed.
        lk.unlock();
        ::kill(pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (::waitpid(pid, nullptr, WNOHANG) == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        lk.lock();
    }
#endif
    s.state = ProcessState::Killed;
    s.ended_at = std::chrono::system_clock::now();
    s.updated_at = s.ended_at;
    impl_->finished[id] = std::move(s);
}

void ProcessRegistry::feed_output(const std::string& id,
                                  std::string_view chunk) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    auto it = impl_->running.find(id);
    if (it == impl_->running.end()) return;
    ProcessSession& s = it->second;
    s.append_output(chunk);
    s.updated_at = std::chrono::system_clock::now();
    impl_->scan_chunk(s, chunk);

    // If the scanner flipped state to Killed (overload) move it to
    // finished so further feed_output calls are a no-op.
    if (s.state == ProcessState::Killed) {
        ProcessSession moved = std::move(s);
        impl_->running.erase(it);
        impl_->finished[id] = std::move(moved);
    }
}

std::vector<WatchNotification> ProcessRegistry::drain_notifications() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::vector<WatchNotification> out(impl_->notifications.begin(),
                                       impl_->notifications.end());
    impl_->notifications.clear();
    return out;
}

// ---- Persistence --------------------------------------------------------

void ProcessRegistry::checkpoint() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json running = nlohmann::json::array();
    nlohmann::json finished = nlohmann::json::array();
    for (const auto& [_, s] : impl_->running)
        running.push_back(serialize_session(s));
    for (const auto& [_, s] : impl_->finished)
        finished.push_back(serialize_session(s));
    root["running"] = std::move(running);
    root["finished"] = std::move(finished);

    std::error_code ec;
    std::filesystem::create_directories(impl_->checkpoint_path.parent_path(),
                                        ec);
    (void)ec;
    hermes::core::atomic_io::atomic_write(impl_->checkpoint_path, root.dump(2));
}

void ProcessRegistry::restore_from_checkpoint() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->running.clear();
    impl_->finished.clear();
    impl_->orphaned.clear();

    auto raw = hermes::core::atomic_io::atomic_read(impl_->checkpoint_path);
    if (!raw) return;
    auto root = nlohmann::json::parse(*raw, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;

    if (root.contains("running") && root["running"].is_array()) {
        for (const auto& entry : root["running"]) {
            ProcessSession s = deserialize_session(entry);
            // A Running process on a previous run is orphaned — the
            // gateway needs to reconcile.
            impl_->orphaned.push_back(std::move(s));
        }
    }
    if (root.contains("finished") && root["finished"].is_array()) {
        for (const auto& entry : root["finished"]) {
            ProcessSession s = deserialize_session(entry);
            std::string id = s.id;
            impl_->finished[id] = std::move(s);
        }
    }
}

std::vector<ProcessSession> ProcessRegistry::orphaned() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->orphaned;
}

}  // namespace hermes::state
