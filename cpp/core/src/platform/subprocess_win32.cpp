// Win32 implementation of hermes::core::platform::run_capture().
#if defined(_WIN32)

#include "hermes/core/platform/subprocess.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <sstream>
#include <string>

namespace hermes::core::platform {

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

// Quote an argv element for CreateProcessW's command-line parsing.
std::wstring quote_arg(const std::string& a) {
    auto w = utf8_to_wide(a);
    bool needs_quotes = w.empty() ||
        w.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) return w;
    std::wstring out;
    out.reserve(w.size() + 2);
    out.push_back(L'"');
    std::size_t slashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            out.push_back(c);
            slashes = 0;
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_cmdline(const std::vector<std::string>& argv) {
    std::wstring out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out.push_back(L' ');
        out += quote_arg(argv[i]);
    }
    return out;
}

// Build environment block: inherited env + extra_env overrides.
std::wstring build_env_block(const std::vector<std::string>& extra) {
    if (extra.empty()) return {};
    LPWCH env = ::GetEnvironmentStringsW();
    std::wstring block;
    if (env) {
        for (LPCWCH p = env; *p;) {
            std::wstring entry = p;
            block += entry;
            block.push_back(L'\0');
            p += entry.size() + 1;
        }
        ::FreeEnvironmentStringsW(env);
    }
    for (const auto& kv : extra) block += utf8_to_wide(kv) + L'\0';
    block.push_back(L'\0');
    return block;
}

}  // namespace

SubprocessResult run_capture(const SubprocessOptions& opts) {
    SubprocessResult r;
    if (opts.argv.empty() || opts.argv[0].empty()) {
        r.spawn_error = "run_capture: argv is empty";
        return r;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = nullptr, in_w = nullptr;
    HANDLE out_r = nullptr, out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;

    if (!::CreatePipe(&in_r, &in_w, &sa, 0) ||
        !::CreatePipe(&out_r, &out_w, &sa, 0) ||
        !::CreatePipe(&err_r, &err_w, &sa, 0)) {
        r.spawn_error = "CreatePipe failed";
        if (in_r) ::CloseHandle(in_r);
        if (in_w) ::CloseHandle(in_w);
        if (out_r) ::CloseHandle(out_r);
        if (out_w) ::CloseHandle(out_w);
        if (err_r) ::CloseHandle(err_r);
        if (err_w) ::CloseHandle(err_w);
        return r;
    }
    // Parent-side handles must not be inherited.
    ::SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    if (opts.discard_output) {
        HANDLE nul = ::CreateFileW(L"NUL", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, nullptr);
        si.hStdOutput = nul;
        si.hStdError = nul;
    } else {
        si.hStdOutput = out_w;
        si.hStdError = err_w;
    }

    PROCESS_INFORMATION pi{};
    auto cmdline = build_cmdline(opts.argv);
    auto env_block = build_env_block(opts.extra_env);
    auto cwd_w = utf8_to_wide(opts.cwd);

    BOOL ok = ::CreateProcessW(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        env_block.empty() ? nullptr : env_block.data(),
        opts.cwd.empty() ? nullptr : cwd_w.c_str(),
        &si, &pi);

    ::CloseHandle(in_r);
    ::CloseHandle(out_w);
    ::CloseHandle(err_w);

    if (!ok) {
        r.spawn_error = "CreateProcessW failed: " +
                        std::to_string(::GetLastError());
        ::CloseHandle(in_w);
        ::CloseHandle(out_r);
        ::CloseHandle(err_r);
        return r;
    }

    // Feed stdin, then close.
    if (!opts.stdin_input.empty()) {
        DWORD written = 0;
        ::WriteFile(in_w, opts.stdin_input.data(),
                    static_cast<DWORD>(opts.stdin_input.size()),
                    &written, nullptr);
    }
    ::CloseHandle(in_w);

    auto drain = [](HANDLE h, std::string& sink) {
        char buf[4096];
        DWORD n = 0;
        while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            sink.append(buf, n);
        }
    };

    // Blocking drain of stdout/stderr (they close when the child exits).
    drain(out_r, r.stdout_text);
    drain(err_r, r.stderr_text);

    DWORD wait_ms = opts.timeout.count() > 0
                        ? static_cast<DWORD>(opts.timeout.count())
                        : INFINITE;
    DWORD wr = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wr == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        r.timed_out = true;
        ::WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    if (::GetExitCodeProcess(pi.hProcess, &exit_code)) {
        r.exit_code = static_cast<int>(exit_code);
    }

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(out_r);
    ::CloseHandle(err_r);

    return r;
}

}  // namespace hermes::core::platform

#endif  // _WIN32
