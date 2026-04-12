#include "hermes/environments/local.hpp"
#include "hermes/environments/env_filter.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdexcept>
#include <vector>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

// PTY support on Linux and macOS.
#if defined(__linux__) || defined(__APPLE__)
#include <pty.h>
#include <utmp.h>
#define HERMES_HAS_PTY 1
#else
#define HERMES_HAS_PTY 0
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace hermes::environments {

#ifdef _WIN32

LocalEnvironment::LocalEnvironment() {}
LocalEnvironment::~LocalEnvironment() = default;

namespace {

// Execute a command on Windows using CreatePipe + CreateProcess (non-PTY).
// When opts.use_pty is true, we attempt CreatePseudoConsole (ConPTY) which is
// available on Windows 10 1809+. Both paths share the same read/timeout loop.
CompletedProcess execute_win32(const std::string& command,
                               const ExecuteOptions& opts) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    CompletedProcess result;

    // Build the "cmd.exe /C <command>" invocation.
    std::string cmdline = "cmd.exe /C " + command;
    std::vector<char> cmd_mut(cmdline.begin(), cmdline.end());
    cmd_mut.push_back('\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdout_rd = nullptr, stdout_wr = nullptr;
    HANDLE stderr_rd = nullptr, stderr_wr = nullptr;

    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        result.exit_code = -1;
        return result;
    }
    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    // ConPTY path: CreatePseudoConsole + EXTENDED_STARTUPINFO_PRESENT.
    HPCON hpc = nullptr;
    bool using_pty = false;
    if (opts.use_pty) {
        HANDLE pty_in_rd = nullptr, pty_in_wr = nullptr;
        HANDLE pty_out_rd = nullptr, pty_out_wr = nullptr;
        if (CreatePipe(&pty_in_rd, &pty_in_wr, nullptr, 0) &&
            CreatePipe(&pty_out_rd, &pty_out_wr, nullptr, 0)) {
            COORD size{80, 25};
            HRESULT hr = CreatePseudoConsole(size, pty_in_rd, pty_out_wr, 0, &hpc);
            if (SUCCEEDED(hr)) {
                using_pty = true;
                // Replace stdout/stderr readers with the PTY output reader.
                CloseHandle(stdout_rd); CloseHandle(stdout_wr);
                CloseHandle(stderr_rd); CloseHandle(stderr_wr);
                stdout_rd = pty_out_rd;
                stdout_wr = nullptr;
                stderr_rd = nullptr;
                stderr_wr = nullptr;
                CloseHandle(pty_in_rd);
                CloseHandle(pty_in_wr);
                CloseHandle(pty_out_wr);
            } else {
                CloseHandle(pty_in_rd); CloseHandle(pty_in_wr);
                CloseHandle(pty_out_rd); CloseHandle(pty_out_wr);
            }
        }
    }

    STARTUPINFOEXA si_ex{};
    si_ex.StartupInfo.cb = sizeof(si_ex);
    DWORD creation_flags = 0;
    std::vector<BYTE> attr_buf;

    if (using_pty) {
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attr_buf.resize(attr_size);
        si_ex.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        InitializeProcThreadAttributeList(si_ex.lpAttributeList, 1, 0, &attr_size);
        UpdateProcThreadAttribute(si_ex.lpAttributeList, 0,
                                  PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                  hpc, sizeof(hpc), nullptr, nullptr);
        creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    } else {
        si_ex.StartupInfo.hStdOutput = stdout_wr;
        si_ex.StartupInfo.hStdError = stderr_wr;
        si_ex.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si_ex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    }

    PROCESS_INFORMATION pi{};
    const char* cwd_cstr = opts.cwd.empty() ? nullptr : opts.cwd.string().c_str();

    BOOL ok = CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE,
                             creation_flags, nullptr, cwd_cstr,
                             &si_ex.StartupInfo, &pi);
    if (!ok) {
        if (stdout_wr) CloseHandle(stdout_wr);
        if (stderr_wr) CloseHandle(stderr_wr);
        if (stdout_rd) CloseHandle(stdout_rd);
        if (stderr_rd) CloseHandle(stderr_rd);
        if (hpc) ClosePseudoConsole(hpc);
        if (!attr_buf.empty())
            DeleteProcThreadAttributeList(si_ex.lpAttributeList);
        result.exit_code = -1;
        return result;
    }

    // Close write ends in parent so reads get EOF when child exits.
    if (stdout_wr) { CloseHandle(stdout_wr); stdout_wr = nullptr; }
    if (stderr_wr) { CloseHandle(stderr_wr); stderr_wr = nullptr; }

    auto deadline = start + opts.timeout;
    auto read_available = [](HANDLE h, std::string& out) {
        if (!h) return true;
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail == 0) return true;
        char buf[4096];
        DWORD got = 0;
        DWORD to_read = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        if (ReadFile(h, buf, to_read, &got, nullptr) && got > 0) {
            out.append(buf, got);
        }
        return true;
    };

    while (true) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        read_available(stdout_rd, result.stdout_text);
        read_available(stderr_rd, result.stderr_text);
        if (wait == WAIT_OBJECT_0) break;
        if (opts.cancel_fn && opts.cancel_fn()) {
            TerminateProcess(pi.hProcess, 1);
            result.interrupted = true;
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
        if (clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            result.timed_out = true;
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
    }

    // Drain remaining output.
    read_available(stdout_rd, result.stdout_text);
    read_available(stderr_rd, result.stderr_text);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    } else {
        result.exit_code = -1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (stdout_rd) CloseHandle(stdout_rd);
    if (stderr_rd) CloseHandle(stderr_rd);
    if (hpc) ClosePseudoConsole(hpc);
    if (!attr_buf.empty())
        DeleteProcThreadAttributeList(si_ex.lpAttributeList);

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - start);
    return result;
}

}  // namespace

CompletedProcess LocalEnvironment::execute(const std::string& cmd,
                                           const ExecuteOptions& opts) {
    return execute_win32(cmd, opts);
}

#else

LocalEnvironment::LocalEnvironment()
    : cwd_tracker_(std::make_unique<FileCwdTracker>()) {}

LocalEnvironment::~LocalEnvironment() = default;

namespace {

void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

// Shared watchdog thread logic used by both pipe and PTY code paths.
struct WatchdogState {
    std::atomic<bool> child_done{false};
    std::atomic<bool> do_cancel{false};
    std::atomic<bool> do_timeout{false};
};

void run_watchdog(WatchdogState& state, pid_t child,
                  std::chrono::steady_clock::time_point start,
                  const ExecuteOptions& opts) {
    auto deadline = start + opts.timeout;
    while (!state.child_done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (opts.cancel_fn && opts.cancel_fn()) {
            state.do_cancel.store(true, std::memory_order_relaxed);
            ::kill(child, SIGTERM);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!state.child_done.load(std::memory_order_relaxed)) {
                ::kill(child, SIGKILL);
            }
            return;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            state.do_timeout.store(true, std::memory_order_relaxed);
            ::kill(child, SIGTERM);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!state.child_done.load(std::memory_order_relaxed)) {
                ::kill(child, SIGKILL);
            }
            return;
        }
    }
}

CompletedProcess execute_pipe(const std::string& wrapped_cmd,
                              const std::filesystem::path& cwd,
                              const std::unordered_map<std::string, std::string>& filtered_env,
                              const ExecuteOptions& opts,
                              std::chrono::steady_clock::time_point start,
                              FileCwdTracker* cwd_tracker) {
    CompletedProcess result;

    // Build envp array.
    std::vector<std::string> env_strings;
    env_strings.reserve(filtered_env.size());
    for (const auto& [k, v] : filtered_env) {
        env_strings.push_back(k + "=" + v);
    }
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    // Create pipes for stdout and stderr.
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.exit_code = -1;
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        result.exit_code = -1;
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        return result;
    }

    if (child == 0) {
        // Child process.
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        if (!cwd.empty()) {
            if (::chdir(cwd.string().c_str()) != 0) {
                _exit(127);
            }
        }

        // Build argv.
        const char* argv[] = {"bash", "-c", wrapped_cmd.c_str(), nullptr};

        if (!filtered_env.empty()) {
            ::execve("/bin/bash", const_cast<char* const*>(argv), envp.data());
        } else {
            ::execv("/bin/bash", const_cast<char* const*>(argv));
        }
        _exit(127);
    }

    // Parent process.
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    set_nonblock(stdout_pipe[0]);
    set_nonblock(stderr_pipe[0]);

    WatchdogState wdog;

    std::thread watchdog([&]() {
        run_watchdog(wdog, child, start, opts);
    });

    // Read stdout/stderr via poll.
    struct pollfd fds[2];
    fds[0].fd = stdout_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = stderr_pipe[0];
    fds[1].events = POLLIN;

    int open_fds = 2;
    while (open_fds > 0) {
        int ret = ::poll(fds, 2, 100);
        if (ret > 0) {
            if (fds[0].revents & (POLLIN | POLLHUP)) {
                char buf[4096];
                auto n = ::read(fds[0].fd, buf, sizeof(buf));
                if (n > 0) {
                    result.stdout_text.append(buf, static_cast<std::size_t>(n));
                } else if (n == 0) {
                    fds[0].fd = -1;
                    --open_fds;
                }
            }
            if (fds[1].revents & (POLLIN | POLLHUP)) {
                char buf[4096];
                auto n = ::read(fds[1].fd, buf, sizeof(buf));
                if (n > 0) {
                    result.stderr_text.append(buf, static_cast<std::size_t>(n));
                } else if (n == 0) {
                    fds[1].fd = -1;
                    --open_fds;
                }
            }
        }
        // Check for closed fds.
        if (fds[0].revents & POLLNVAL) { fds[0].fd = -1; --open_fds; }
        if (fds[1].revents & POLLNVAL) { fds[1].fd = -1; --open_fds; }
    }

    // Wait for the child.
    int status = 0;
    ::waitpid(child, &status, 0);
    wdog.child_done.store(true, std::memory_order_relaxed);
    watchdog.join();

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    result.timed_out = wdog.do_timeout.load();
    result.interrupted = wdog.do_cancel.load();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Extract final cwd from tracker.
    result.final_cwd = cwd_tracker->after_run(result.stdout_text);

    return result;
}

#if HERMES_HAS_PTY

CompletedProcess execute_pty(const std::string& wrapped_cmd,
                             const std::filesystem::path& cwd,
                             const std::unordered_map<std::string, std::string>& filtered_env,
                             const ExecuteOptions& opts,
                             std::chrono::steady_clock::time_point start,
                             FileCwdTracker* cwd_tracker) {
    CompletedProcess result;

    // Build envp array.
    std::vector<std::string> env_strings;
    env_strings.reserve(filtered_env.size());
    for (const auto& [k, v] : filtered_env) {
        env_strings.push_back(k + "=" + v);
    }
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    int master_fd = -1;
    pid_t child = forkpty(&master_fd, nullptr, nullptr, nullptr);

    if (child < 0) {
        result.exit_code = -1;
        return result;
    }

    if (child == 0) {
        // Child process — runs inside PTY slave.
        if (!cwd.empty()) {
            if (::chdir(cwd.string().c_str()) != 0) {
                _exit(127);
            }
        }

        const char* argv[] = {"bash", "-c", wrapped_cmd.c_str(), nullptr};

        if (!filtered_env.empty()) {
            ::execve("/bin/bash", const_cast<char* const*>(argv), envp.data());
        } else {
            ::execv("/bin/bash", const_cast<char* const*>(argv));
        }
        _exit(127);
    }

    // Parent: read from master_fd.
    set_nonblock(master_fd);

    WatchdogState wdog;
    std::thread watchdog([&]() {
        run_watchdog(wdog, child, start, opts);
    });

    // Read from master PTY.  PTY merges stdout and stderr into a single
    // stream — this is the expected behaviour of a real terminal.
    struct pollfd pfd;
    pfd.fd = master_fd;
    pfd.events = POLLIN;

    bool eof = false;
    while (!eof) {
        int ret = ::poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            auto n = ::read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                result.stdout_text.append(buf, static_cast<std::size_t>(n));
            } else {
                eof = true;
            }
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            eof = true;
        }
        // On some systems EIO is returned when the slave side closes.
        if (ret > 0 && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
            eof = true;
        }
    }

    // Wait for child.
    int status = 0;
    ::waitpid(child, &status, 0);
    wdog.child_done.store(true, std::memory_order_relaxed);
    watchdog.join();

    ::close(master_fd);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    result.timed_out = wdog.do_timeout.load();
    result.interrupted = wdog.do_cancel.load();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Strip carriage returns that the PTY layer inserts (\r\n -> \n).
    std::string cleaned;
    cleaned.reserve(result.stdout_text.size());
    for (std::size_t i = 0; i < result.stdout_text.size(); ++i) {
        if (result.stdout_text[i] == '\r' &&
            i + 1 < result.stdout_text.size() &&
            result.stdout_text[i + 1] == '\n') {
            continue;  // skip \r, the \n will be added next iteration
        }
        cleaned.push_back(result.stdout_text[i]);
    }
    result.stdout_text = std::move(cleaned);

    result.final_cwd = cwd_tracker->after_run(result.stdout_text);

    return result;
}

#endif  // HERMES_HAS_PTY

}  // namespace

CompletedProcess LocalEnvironment::execute(const std::string& cmd,
                                           const ExecuteOptions& opts) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    // Determine working directory.
    auto cwd = opts.cwd.empty() ? std::filesystem::current_path() : opts.cwd;

    // Prepare command with cwd tracking.
    std::string wrapped_cmd = cwd_tracker_->before_run(cmd, cwd);

    // Filter environment.
    auto filtered_env = filter_env(opts.env_vars);

#if HERMES_HAS_PTY
    if (opts.use_pty) {
        return execute_pty(wrapped_cmd, cwd, filtered_env, opts, start,
                           cwd_tracker_.get());
    }
#endif

    return execute_pipe(wrapped_cmd, cwd, filtered_env, opts, start,
                        cwd_tracker_.get());
}

#endif  // _WIN32

}  // namespace hermes::environments
