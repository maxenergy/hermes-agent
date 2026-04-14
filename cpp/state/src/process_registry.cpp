#include "hermes/state/process_registry.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
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
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

void ProcessSession::append_stream(std::string_view chunk, int stream) {
    append_output(chunk);
    auto* buf = (stream == 2) ? &stderr_buffer : &stdout_buffer;
    buf->append(chunk.data(), chunk.size());
    if (buf->size() > stream_buffer_max) {
        std::size_t drop = buf->size() - stream_buffer_max;
        buf->erase(0, drop);
    }
}

std::string ProcessSession::tail(int stream, std::size_t n) const {
    const std::string* src = nullptr;
    if (stream == 1) src = &stdout_buffer;
    else if (stream == 2) src = &stderr_buffer;
    else src = &output_buffer;
    if (!src || src->size() <= n) return src ? *src : std::string();
    return src->substr(src->size() - n);
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
    // Per-process restart policy (keyed by session id).  Consulted by
    // ProcessRegistry::maybe_restart.
    std::unordered_map<std::string, RestartPolicy> restart_policies;
    // Persisted-bytes watermark — tracks what has already been flushed
    // to the external sink so persist_tail only emits new bytes.
    // (The field on ProcessSession is authoritative; this map is a
    // defensive cache keyed by id for processes that have since been
    // moved to `finished`.)
    std::unordered_map<std::string, std::size_t> persisted_watermark;

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

// -------------------------------------------------------------------------
// spawn_local — real POSIX fork/exec integration.
//
// Equivalent to Python `process_registry.spawn_local()`: forks a child
// that runs `<shell> -c <command>` with pipes connected to stdout/stderr,
// registers the ProcessSession as Running, and spins up two background
// threads:
//
//   * reader thread: drains stdout+stderr pipes via poll() and pushes
//     chunks into feed_output() so watch_patterns fire live.
//   * waiter thread: waitpid()s the child, marks exited/killed with the
//     real exit code, reaps the zombie.  Optionally enforces timeout.
//
// The child is placed in its own process group (setsid) so kill() can
// signal everything it spawned — matching the Python reference's
// os.setsid + os.killpg() pattern.
//
// We deliberately use raw POSIX here rather than boost::process: the
// hermes_state library does not otherwise link Boost, and our feature
// surface (cwd / env / timeout / streaming / pgid kill) is a direct
// fit for fork/exec.  local.cpp uses the same pattern for foreground
// execution; spawn_local is the background analogue.
// -------------------------------------------------------------------------
#ifndef _WIN32
namespace {

// Build a merged envp suitable for execve().  When `extra` is empty the
// child simply inherits the parent environment via environ.
std::vector<std::string>
build_env_strings(const std::unordered_map<std::string, std::string>& extra) {
    std::unordered_map<std::string, std::string> merged;
    for (char** e = environ; e && *e; ++e) {
        std::string kv = *e;
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        merged[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const auto& [k, v] : extra) {
        merged[k] = v;
    }
    std::vector<std::string> out;
    out.reserve(merged.size());
    for (const auto& [k, v] : merged) {
        out.push_back(k + "=" + v);
    }
    return out;
}

void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

std::string ProcessRegistry::spawn_local(const SpawnOptions& opts) {
    // Create pipes BEFORE the session so we can fail fast.
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        if (out_pipe[0] >= 0) ::close(out_pipe[0]);
        if (out_pipe[1] >= 0) ::close(out_pipe[1]);
        if (err_pipe[0] >= 0) ::close(err_pipe[0]);
        if (err_pipe[1] >= 0) ::close(err_pipe[1]);
        // Register a session that already failed so the caller has
        // something to reference.
        ProcessSession failed;
        failed.id = make_id();
        failed.command = opts.command;
        failed.task_id = opts.task_id;
        failed.session_key = opts.session_key;
        failed.cwd = opts.cwd.empty() ? std::filesystem::current_path()
                                      : opts.cwd;
        failed.watch_patterns = opts.watch_patterns;
        failed.started_at = std::chrono::system_clock::now();
        failed.updated_at = failed.started_at;
        failed.ended_at = failed.started_at;
        failed.state = ProcessState::Exited;
        failed.exit_code = -1;
        std::string id = failed.id;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->finished[id] = std::move(failed);
        }
        return id;
    }

    // Resolve cwd / shell.
    std::filesystem::path resolved_cwd = opts.cwd.empty()
        ? std::filesystem::current_path()
        : opts.cwd;
    std::string shell = opts.shell.empty() ? "/bin/sh" : opts.shell;

    // Compose envp up front (heap) so the child can execve it.
    std::vector<std::string> env_storage = build_env_strings(opts.env_vars);
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);

    // Build argv.  The child runs `<shell> -c <command>` so shell
    // features (pipes, expansions, backgrounding) keep working.
    std::string shell_copy = shell;
    std::string flag = "-c";
    std::string cmd_copy = opts.command;
    std::vector<char*> argv{shell_copy.data(), flag.data(),
                            cmd_copy.data(), nullptr};

    pid_t child = ::fork();
    if (child < 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        ProcessSession failed;
        failed.id = make_id();
        failed.command = opts.command;
        failed.cwd = resolved_cwd;
        failed.started_at = std::chrono::system_clock::now();
        failed.updated_at = failed.started_at;
        failed.ended_at = failed.started_at;
        failed.state = ProcessState::Exited;
        failed.exit_code = -1;
        std::string id = failed.id;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->finished[id] = std::move(failed);
        }
        return id;
    }

    if (child == 0) {
        // ---- Child ----
        // New process group so the parent can signal the whole tree.
        ::setsid();

        // Apply per-process resource limits.  Failures here are
        // non-fatal — the shell will run with the parent's limits.
        const ResourceLimits& lim = opts.limits;
        if (lim.cpu_seconds > 0) {
            rlimit r{lim.cpu_seconds, lim.cpu_seconds};
            ::setrlimit(RLIMIT_CPU, &r);
        }
        if (lim.memory_bytes > 0) {
            rlimit r{lim.memory_bytes, lim.memory_bytes};
            ::setrlimit(RLIMIT_AS, &r);
        }
        if (lim.max_open_files > 0) {
            rlimit r{lim.max_open_files, lim.max_open_files};
            ::setrlimit(RLIMIT_NOFILE, &r);
        }
        if (lim.disable_core_dumps) {
            rlimit r{0, 0};
            ::setrlimit(RLIMIT_CORE, &r);
        }

        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        if (!resolved_cwd.empty()) {
            if (::chdir(resolved_cwd.string().c_str()) != 0) {
                _exit(127);
            }
        }

        ::execve(shell.c_str(), argv.data(), envp.data());
        _exit(127);  // execve failed
    }

    // ---- Parent ----
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    ProcessSession session;
    session.id = make_id();
    session.command = opts.command;
    session.task_id = opts.task_id;
    session.session_key = opts.session_key;
    session.watch_patterns = opts.watch_patterns;
    session.cwd = resolved_cwd;
    session.pid = static_cast<int>(child);
    session.pid_scope = PidScope::Host;
    session.started_at = std::chrono::system_clock::now();
    session.updated_at = session.started_at;
    session.state = ProcessState::Running;

    std::string id = session.id;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->running[id] = std::move(session);
        if (opts.restart.max_restarts > 0) {
            impl_->restart_policies[id] = opts.restart;
        }
    }

    // Reader thread: poll stdout/stderr pipes until both EOF, feeding
    // chunks into the registry.  Detached so it cleans up independently.
    ProcessRegistry* self = this;
    int out_fd = out_pipe[0];
    int err_fd = err_pipe[0];
    auto persist_sink = opts.persist_sink;
    std::thread([self, id, out_fd, err_fd, persist_sink]() {
        struct pollfd fds[2];
        fds[0].fd = out_fd; fds[0].events = POLLIN;
        fds[1].fd = err_fd; fds[1].events = POLLIN;
        int open_fds = 2;
        while (open_fds > 0) {
            int ret = ::poll(fds, 2, 100);
            if (ret > 0) {
                for (int i = 0; i < 2; ++i) {
                    if (fds[i].fd < 0) continue;
                    if (fds[i].revents & (POLLIN | POLLHUP)) {
                        char buf[4096];
                        ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
                        if (n > 0) {
                            int stream = (i == 0) ? 1 : 2;
                            std::string_view chunk(
                                buf, static_cast<std::size_t>(n));
                            self->feed_output_stream(id, chunk, stream);
                            if (persist_sink) {
                                try {
                                    persist_sink(id, stream, chunk);
                                } catch (...) {}
                            }
                        } else if (n == 0) {
                            ::close(fds[i].fd);
                            fds[i].fd = -1;
                            --open_fds;
                        }
                    }
                    if (fds[i].revents & (POLLERR | POLLNVAL)) {
                        if (fds[i].fd >= 0) ::close(fds[i].fd);
                        fds[i].fd = -1;
                        --open_fds;
                    }
                }
            }
        }
    }).detach();

    // Waiter thread: waitpid() + timeout enforcement.  Writes the final
    // exit code into the session, reaps the zombie.
    std::chrono::seconds timeout = opts.timeout;
    std::thread([self, id, child, timeout]() {
        auto start = std::chrono::steady_clock::now();
        bool killed_for_timeout = false;
        while (true) {
            int status = 0;
            pid_t r = ::waitpid(child, &status, WNOHANG);
            if (r == child) {
                int exit_code;
                if (WIFEXITED(status)) {
                    exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    exit_code = 128 + WTERMSIG(status);
                } else {
                    exit_code = -1;
                }
                if (killed_for_timeout) {
                    // Match Python reference: timeout → exit_code 124.
                    self->mark_exited(id, 124);
                } else {
                    self->mark_exited(id, exit_code);
                }
                return;
            }
            if (r < 0 && errno != EINTR) {
                // Child vanished (shouldn't happen — we hold the parent
                // role); treat as exited with -1 so the session doesn't
                // leak.
                self->mark_exited(id, -1);
                return;
            }
            if (timeout.count() > 0 && !killed_for_timeout &&
                (std::chrono::steady_clock::now() - start) >= timeout) {
                killed_for_timeout = true;
                // SIGTERM the whole group; escalate after 2s.
                ::killpg(child, SIGTERM);
                std::thread([child]() {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    // Best-effort escalation.  If the child already
                    // exited waitpid will reap it and this is a no-op.
                    ::killpg(child, SIGKILL);
                }).detach();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();

    return id;
}
#else   // _WIN32
std::string ProcessRegistry::spawn_local(const SpawnOptions& opts) {
    // The Windows agent owns this path — see cpp/environments/src/local.cpp
    // for the CreateProcess/ConPTY template.
    ProcessSession failed;
    failed.id = make_id();
    failed.command = opts.command;
    failed.started_at = std::chrono::system_clock::now();
    failed.updated_at = failed.started_at;
    failed.ended_at = failed.started_at;
    failed.state = ProcessState::Exited;
    failed.exit_code = -1;
    std::string id = failed.id;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->finished[id] = std::move(failed);
    }
    return id;
}
#endif  // _WIN32

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
#else
    // Windows: no graceful SIGTERM equivalent for arbitrary child processes —
    // TerminateProcess is the canonical way to force-exit by PID.  We still
    // give the process a brief grace window in case cooperative shutdown was
    // initiated elsewhere (e.g. via a console ctrl event above this layer).
    // TODO(win): verify on Windows CI — specifically that OpenProcess with
    // PROCESS_TERMINATE succeeds for children spawned via CreateProcess in
    // local.cpp, and that the 2s grace window matches POSIX semantics.
    if (s.pid) {
        DWORD pid = static_cast<DWORD>(*s.pid);
        lk.unlock();
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h != nullptr) {
            // Grace period: if the process exits on its own, skip Terminate.
            DWORD wait = ::WaitForSingleObject(h, 2000);
            if (wait == WAIT_TIMEOUT) {
                ::TerminateProcess(h, 1);
                ::WaitForSingleObject(h, 5000);
            }
            ::CloseHandle(h);
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

// -------------------------------------------------------------------------
// Stream-aware feed_output + tails
// -------------------------------------------------------------------------

void ProcessRegistry::feed_output_stream(const std::string& id,
                                         std::string_view chunk, int stream) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    auto it = impl_->running.find(id);
    if (it == impl_->running.end()) return;
    ProcessSession& s = it->second;
    s.append_stream(chunk, stream);
    s.updated_at = std::chrono::system_clock::now();
    impl_->scan_chunk(s, chunk);
    if (s.state == ProcessState::Killed) {
        ProcessSession moved = std::move(s);
        impl_->running.erase(it);
        impl_->finished[id] = std::move(moved);
    }
}

std::string ProcessRegistry::tail(const std::string& id, int stream,
                                  std::size_t n) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    auto run_it = impl_->running.find(id);
    if (run_it != impl_->running.end()) return run_it->second.tail(stream, n);
    auto fin_it = impl_->finished.find(id);
    if (fin_it != impl_->finished.end()) return fin_it->second.tail(stream, n);
    return {};
}

std::size_t ProcessRegistry::persist_tail(
    const std::string& id,
    const std::function<void(std::string_view)>& sink) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    ProcessSession* s = nullptr;
    auto run_it = impl_->running.find(id);
    if (run_it != impl_->running.end()) s = &run_it->second;
    else {
        auto fin_it = impl_->finished.find(id);
        if (fin_it != impl_->finished.end()) s = &fin_it->second;
    }
    if (!s) return 0;

    // Total bytes observed so far = bytes in buffer + bytes previously
    // dropped from the front.  We approximate "total observed" by
    // tracking the maximum watermark seen; because the buffer is
    // rolling we can only safely sink what's currently in it starting
    // from the last persisted offset (which may have scrolled off).
    std::size_t already = s->persisted_bytes;
    std::size_t buffer_len = s->output_buffer.size();
    if (buffer_len <= already) {
        // Either we've already flushed everything, or older bytes
        // rolled off — treat persisted_bytes as "end of previous
        // flush" and emit the full current buffer as new material.
        if (buffer_len == 0) return 0;
        if (already == buffer_len) return 0;
    }
    std::string_view new_bytes;
    if (buffer_len > already) {
        new_bytes = std::string_view(s->output_buffer.data() + already,
                                     buffer_len - already);
    } else {
        new_bytes = std::string_view(s->output_buffer);
    }
    if (new_bytes.empty()) return 0;
    try {
        if (sink) sink(new_bytes);
    } catch (...) {
        // Don't propagate — gateway-side sinks may throw under DB
        // contention, but we mustn't corrupt the registry state.
    }
    s->persisted_bytes = buffer_len;
    impl_->persisted_watermark[id] = buffer_len;
    return new_bytes.size();
}

// -------------------------------------------------------------------------
// Restart policy bookkeeping
// -------------------------------------------------------------------------

void ProcessRegistry::set_restart_policy(const std::string& id,
                                         RestartPolicy policy) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->restart_policies[id] = std::move(policy);
}

RestartPolicy ProcessRegistry::restart_policy(const std::string& id) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    auto it = impl_->restart_policies.find(id);
    if (it == impl_->restart_policies.end()) return {};
    return it->second;
}

bool ProcessRegistry::maybe_restart(const std::string& id, int exit_code) {
    std::unique_lock<std::mutex> lk(impl_->mtx);
    auto pol_it = impl_->restart_policies.find(id);
    if (pol_it == impl_->restart_policies.end()) return false;
    RestartPolicy& policy = pol_it->second;
    if (policy.max_restarts <= 0) return false;

    // Find the session (may be running if we're coming from feed_output
    // synthesis, or finished if we're called post-waitpid).
    ProcessSession* s = nullptr;
    auto run_it = impl_->running.find(id);
    if (run_it != impl_->running.end()) s = &run_it->second;
    else {
        auto fin_it = impl_->finished.find(id);
        if (fin_it != impl_->finished.end()) s = &fin_it->second;
    }
    if (!s) return false;

    if (s->restart_attempts >= policy.max_restarts) return false;

    // Exit-code gating.
    bool restartable = false;
    if (exit_code == 124) {
        restartable = policy.restart_on_timeout;
    } else if (policy.restart_on_exit_codes.empty()) {
        restartable = (exit_code != 0);
    } else {
        restartable = std::find(policy.restart_on_exit_codes.begin(),
                                policy.restart_on_exit_codes.end(),
                                exit_code) !=
                      policy.restart_on_exit_codes.end();
    }
    if (!restartable) return false;

    ++s->restart_attempts;
    s->state = ProcessState::Running;
    s->exit_code.reset();
    s->ended_at = {};
    s->updated_at = std::chrono::system_clock::now();
    // Move back to running if currently finished.
    if (impl_->finished.count(id)) {
        ProcessSession moved = std::move(impl_->finished[id]);
        impl_->finished.erase(id);
        impl_->running[id] = std::move(moved);
    }
    return true;
}

}  // namespace hermes::state
