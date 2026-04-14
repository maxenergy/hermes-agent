// main_pipe_mode — pipe / file-input / session restore entry helpers.
// See include/hermes/cli/main_pipe_mode.hpp for the API surface.

#include "hermes/cli/main_pipe_mode.hpp"
#include "hermes/cli/main_preparse.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#else
#include <unistd.h>
#endif

namespace hermes::cli::pipe_mode {

namespace {

// Peek argv for a flag value — non-destructive version used during parsing.
std::string peek_flag(int argc, char** argv,
                      const char* long_name,
                      const char* short_name = nullptr) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == long_name || (short_name && a == short_name)) &&
            i + 1 < argc) {
            return argv[i + 1];
        }
        std::string prefix = std::string(long_name) + "=";
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return {};
}

bool peek_bool_flag(int argc, char** argv,
                    const char* long_name,
                    const char* short_name = nullptr) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == long_name || (short_name && a == short_name)) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// is_pipe_mode
// ---------------------------------------------------------------------------
bool is_pipe_mode() {
    return ::isatty(STDIN_FILENO) == 0;
}

// ---------------------------------------------------------------------------
// read_stdin_to_eof
// ---------------------------------------------------------------------------
std::string read_stdin_to_eof() {
    std::ostringstream buf;
    buf << std::cin.rdbuf();
    return buf.str();
}

// ---------------------------------------------------------------------------
// read_file_contents
// ---------------------------------------------------------------------------
std::string read_file_contents(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open file: " + path;
        return {};
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

// ---------------------------------------------------------------------------
// parse_input_request
// ---------------------------------------------------------------------------
InputRequest parse_input_request(int argc, char** argv) {
    InputRequest req;

    // --input FILE / -i FILE
    std::string input_file = peek_flag(argc, argv, "--input", "-i");
    // --query "..." / -q "..."
    std::string query = peek_flag(argc, argv, "--query", "-q");
    // --image PATH
    req.image_path = peek_flag(argc, argv, "--image");
    req.quiet = peek_bool_flag(argc, argv, "--quiet", "-Q");
    req.verbose = peek_bool_flag(argc, argv, "--verbose", "-v");
    req.pass_session_id = peek_bool_flag(argc, argv, "--pass-session-id");
    req.source_tag = peek_flag(argc, argv, "--source");
    req.resume_session_id = peek_flag(argc, argv, "--resume", "-r");

    std::string max_turns_str = peek_flag(argc, argv, "--max-turns");
    if (!max_turns_str.empty()) {
        try { req.max_turns = std::stoi(max_turns_str); } catch (...) {}
    }

    // --continue [NAME]
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--continue" || a == "-c") {
            req.continue_flag = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                req.continue_name = argv[i + 1];
            }
            break;
        }
        if (a.rfind("--continue=", 0) == 0) {
            req.continue_flag = true;
            req.continue_name = a.substr(11);
            break;
        }
    }

    // Priority: explicit query arg > --input file > pipe stdin > interactive.
    if (!query.empty()) {
        req.source = InputSource::kQueryArg;
        req.payload = query;
    } else if (!input_file.empty()) {
        req.source = InputSource::kFile;
        req.input_file_path = input_file;
        std::string err;
        req.payload = read_file_contents(input_file, &err);
        if (!err.empty() && req.payload.empty()) {
            std::cerr << "hermes: " << err << "\n";
            req.source = InputSource::kNone;
        }
    } else if (is_pipe_mode()) {
        req.source = InputSource::kPipe;
        req.payload = read_stdin_to_eof();
        // Trim trailing newlines.
        while (!req.payload.empty() &&
               (req.payload.back() == '\n' || req.payload.back() == '\r')) {
            req.payload.pop_back();
        }
        if (req.payload.empty()) {
            req.source = InputSource::kNone;
        }
    } else {
        req.source = InputSource::kInteractive;
    }
    return req;
}

// ---------------------------------------------------------------------------
// run_pipe_mode
// ---------------------------------------------------------------------------
int run_pipe_mode(const InputRequest& req,
                  std::function<int(const std::string&)> run_query) {
    if (!run_query) return 2;
    if (req.source == InputSource::kNone) return 0;
    if (req.payload.empty()) return 0;
    return run_query(req.payload);
}

// ---------------------------------------------------------------------------
// PipeSink
// ---------------------------------------------------------------------------
PipeSink::PipeSink() : out_(std::cout) {}
PipeSink::PipeSink(std::ostream& out) : out_(out) {}

void PipeSink::on_text_chunk(const std::string& chunk) {
    if (chunk.empty()) return;
    out_ << chunk;
    out_.flush();
    bytes_ += chunk.size();
    if (!chunk.empty()) {
        last_was_newline_ = (chunk.back() == '\n');
    }
}

void PipeSink::on_tool_call_preview(const std::string& name,
                                    const std::string& args_preview) {
    if (quiet_) return;
    if (!last_was_newline_) {
        out_ << '\n';
        ++bytes_;
    }
    out_ << "[" << name << "] " << args_preview << "\n";
    out_.flush();
    bytes_ += name.size() + args_preview.size() + 4;
    last_was_newline_ = true;
}

void PipeSink::on_tool_result(const std::string& name,
                              const std::string& summary) {
    if (quiet_) return;
    if (!last_was_newline_) {
        out_ << '\n';
        ++bytes_;
    }
    out_ << "[" << name << "] → " << summary << "\n";
    out_.flush();
    bytes_ += name.size() + summary.size() + 6;
    last_was_newline_ = true;
}

void PipeSink::on_turn_boundary() {
    if (quiet_) return;
    if (!last_was_newline_) {
        out_ << '\n';
        ++bytes_;
    }
    out_ << "---\n";
    out_.flush();
    bytes_ += 4;
    last_was_newline_ = true;
}

void PipeSink::finish(const std::string& full_text) {
    if (full_text.empty()) return;
    // If we've been streaming chunks and the concatenation equals the
    // full_text, we've already emitted it.  Otherwise, emit at the end.
    if (bytes_ == 0) {
        out_ << full_text;
        if (!full_text.empty() && full_text.back() != '\n') {
            out_ << '\n';
        }
        out_.flush();
        bytes_ += full_text.size();
    } else if (!last_was_newline_) {
        out_ << '\n';
        out_.flush();
        ++bytes_;
    }
}

// ---------------------------------------------------------------------------
// resolve_session_to_restore
// ---------------------------------------------------------------------------
std::string resolve_session_to_restore(const InputRequest& req) {
    if (!req.resume_session_id.empty()) {
        // Prefer exact ID — let session store validate.
        auto id = hermes::cli::preparse::resolve_session_by_name_or_id(
            req.resume_session_id);
        return id.value_or(req.resume_session_id);
    }
    if (req.continue_flag) {
        if (!req.continue_name.empty()) {
            auto id = hermes::cli::preparse::resolve_session_by_name_or_id(
                req.continue_name);
            return id.value_or("");
        }
        // Empty name means "the last CLI session".
        auto id = hermes::cli::preparse::resolve_last_cli_session();
        return id.value_or("");
    }
    return {};
}

}  // namespace hermes::cli::pipe_mode
