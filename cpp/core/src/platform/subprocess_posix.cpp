// POSIX implementation of hermes::core::platform::run_capture().
#if !defined(_WIN32)

#include "hermes/core/platform/subprocess.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hermes::core::platform {

namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

SubprocessResult run_capture(const SubprocessOptions& opts) {
    SubprocessResult r;
    if (opts.argv.empty() || opts.argv[0].empty()) {
        r.spawn_error = "run_capture: argv is empty";
        return r;
    }

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        r.spawn_error = std::string("pipe: ") + std::strerror(errno);
        close_fd(in_pipe[0]); close_fd(in_pipe[1]);
        close_fd(out_pipe[0]); close_fd(out_pipe[1]);
        close_fd(err_pipe[0]); close_fd(err_pipe[1]);
        return r;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        r.spawn_error = std::string("fork: ") + std::strerror(errno);
        close_fd(in_pipe[0]); close_fd(in_pipe[1]);
        close_fd(out_pipe[0]); close_fd(out_pipe[1]);
        close_fd(err_pipe[0]); close_fd(err_pipe[1]);
        return r;
    }

    if (pid == 0) {
        // Child.
        ::dup2(in_pipe[0], STDIN_FILENO);
        if (opts.discard_output) {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
        } else {
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
        }
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);

        if (!opts.cwd.empty()) {
            if (::chdir(opts.cwd.c_str()) != 0) {
                std::fprintf(stderr, "chdir(%s): %s\n", opts.cwd.c_str(),
                             std::strerror(errno));
                std::_Exit(127);
            }
        }

        for (const auto& kv : opts.extra_env) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(opts.argv.size() + 1);
        for (const auto& a : opts.argv) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(argv[0], argv.data());
        std::fprintf(stderr, "execvp(%s): %s\n", argv[0], std::strerror(errno));
        std::_Exit(127);
    }

    // Parent.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    // Feed stdin and close it.
    if (!opts.stdin_input.empty()) {
        const char* p = opts.stdin_input.data();
        std::size_t remaining = opts.stdin_input.size();
        while (remaining > 0) {
            ssize_t n = ::write(in_pipe[1], p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            p += n;
            remaining -= static_cast<std::size_t>(n);
        }
    }
    ::close(in_pipe[1]);

    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    const auto start = std::chrono::steady_clock::now();
    bool killed = false;

    while (true) {
        int status = 0;
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        bool done = false;
        if (w == pid) {
            if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) r.exit_code = 128 + WTERMSIG(status);
            done = true;
        } else if (w < 0 && errno != EINTR) {
            r.spawn_error = std::string("waitpid: ") + std::strerror(errno);
            done = true;
        }

        pollfd fds[2] = {
            {out_pipe[0], POLLIN, 0},
            {err_pipe[0], POLLIN, 0},
        };
        int pr = ::poll(fds, 2, 50);
        if (pr > 0) {
            char buf[4096];
            if (fds[0].revents & (POLLIN | POLLHUP)) {
                while (true) {
                    ssize_t n = ::read(out_pipe[0], buf, sizeof(buf));
                    if (n > 0) r.stdout_text.append(buf, static_cast<std::size_t>(n));
                    else break;
                }
            }
            if (fds[1].revents & (POLLIN | POLLHUP)) {
                while (true) {
                    ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
                    if (n > 0) r.stderr_text.append(buf, static_cast<std::size_t>(n));
                    else break;
                }
            }
        }

        if (done) break;

        if (opts.timeout.count() > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= opts.timeout && !killed) {
                ::kill(pid, SIGKILL);
                killed = true;
                r.timed_out = true;
            }
        }
    }

    // Drain any remaining buffered output after exit.
    char buf[4096];
    while (true) {
        ssize_t n = ::read(out_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        r.stdout_text.append(buf, static_cast<std::size_t>(n));
    }
    while (true) {
        ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        r.stderr_text.append(buf, static_cast<std::size_t>(n));
    }
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    return r;
}

}  // namespace hermes::core::platform

#endif  // !_WIN32
