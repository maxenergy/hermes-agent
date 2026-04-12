#include "hermes/environments/local.hpp"
#include "hermes/environments/env_filter.hpp"

#ifdef _WIN32
#include <stdexcept>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
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

CompletedProcess LocalEnvironment::execute(const std::string& /*cmd*/,
                                           const ExecuteOptions& /*opts*/) {
    throw std::runtime_error("LocalEnvironment is not supported on Windows");
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

}  // namespace

CompletedProcess LocalEnvironment::execute(const std::string& cmd,
                                           const ExecuteOptions& opts) {
    using clock = std::chrono::steady_clock;

    CompletedProcess result;
    auto start = clock::now();

    // Determine working directory.
    auto cwd = opts.cwd.empty() ? std::filesystem::current_path() : opts.cwd;

    // Prepare command with cwd tracking.
    std::string wrapped_cmd = cwd_tracker_->before_run(cmd, cwd);

    // Filter environment.
    auto filtered_env = filter_env(opts.env_vars);

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

    std::atomic<bool> child_done{false};
    std::atomic<bool> do_cancel{false};
    std::atomic<bool> do_timeout{false};

    // Watchdog thread for timeout and cancellation.
    std::thread watchdog([&]() {
        auto deadline = start + opts.timeout;
        while (!child_done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (opts.cancel_fn && opts.cancel_fn()) {
                do_cancel.store(true, std::memory_order_relaxed);
                ::kill(child, SIGTERM);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!child_done.load(std::memory_order_relaxed)) {
                    ::kill(child, SIGKILL);
                }
                return;
            }

            if (clock::now() >= deadline) {
                do_timeout.store(true, std::memory_order_relaxed);
                ::kill(child, SIGTERM);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!child_done.load(std::memory_order_relaxed)) {
                    ::kill(child, SIGKILL);
                }
                return;
            }
        }
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
    child_done.store(true, std::memory_order_relaxed);
    watchdog.join();

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    result.timed_out = do_timeout.load();
    result.interrupted = do_cancel.load();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - start);

    // Extract final cwd from tracker.
    result.final_cwd = cwd_tracker_->after_run(result.stdout_text);

    return result;
}

#endif  // _WIN32

}  // namespace hermes::environments
