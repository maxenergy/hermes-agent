// main_pipe_mode — pipe / non-TTY / file-input entry points.
//
// Ports the "when stdin is a pipe" branch of hermes_cli/main.py::main().
// When the user runs `cat prompt.txt | hermes` or `hermes --input q.txt`,
// the CLI must bypass the interactive REPL and run a single query.
//
// Also provides session-restore-on-startup semantics matching
// `--resume SESSION` and `--continue NAME`.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::pipe_mode {

// ---------------------------------------------------------------------------
// Input source — what the CLI should use as the query.
// ---------------------------------------------------------------------------
enum class InputSource {
    kInteractive,     // default REPL
    kPipe,            // read stdin to EOF
    kFile,            // --input PATH
    kQueryArg,        // -q "..."
    kNone,            // nothing to do
};

struct InputRequest {
    InputSource source = InputSource::kInteractive;
    std::string payload;         // the resolved prompt (file contents / -q text / stdin)
    std::string image_path;      // optional --image attachment
    std::string input_file_path; // set when source==kFile
    bool quiet = false;          // --quiet / -Q
    bool verbose = false;        // --verbose
    bool pass_session_id = false;
    int max_turns = 0;
    std::string resume_session_id;
    std::string continue_name;
    bool continue_flag = false;
    std::string source_tag;      // session source
};

// Parse argv and environment to build an InputRequest.  Does not consume
// argv — callers still dispatch the subcommand separately.
InputRequest parse_input_request(int argc, char** argv);

// Read stdin to EOF and return as a string.  Returns empty string on error.
std::string read_stdin_to_eof();

// Read the contents of `path` and return as a string.  Returns an empty
// string on error; sets *err if provided.
std::string read_file_contents(const std::string& path,
                               std::string* err = nullptr);

// Decide whether we should switch to non-interactive (pipe) mode.  Mirrors
// Python's `sys.stdin.isatty()` check.
bool is_pipe_mode();

// Entry shim — run the default chat path in pipe mode.  Calls `run_query`
// with the resolved payload.  Returns an exit code.
int run_pipe_mode(const InputRequest& req,
                  std::function<int(const std::string&)> run_query);

// ---------------------------------------------------------------------------
// Streaming display helpers.  The interactive REPL pipes model output
// through a pretty printer; pipe mode just writes it to stdout.  These
// helpers expose the chunking/pipe-sink behavior so tests can drive it.
// ---------------------------------------------------------------------------
class PipeSink {
   public:
    PipeSink();
    explicit PipeSink(std::ostream& out);

    // Called for each model-produced chunk.
    void on_text_chunk(const std::string& chunk);
    void on_tool_call_preview(const std::string& name,
                              const std::string& args_preview);
    void on_tool_result(const std::string& name, const std::string& summary);
    void on_turn_boundary();

    // Emit the final assistant message.
    void finish(const std::string& full_text);

    std::size_t bytes_written() const { return bytes_; }
    bool quiet_mode() const { return quiet_; }
    void set_quiet(bool q) { quiet_ = q; }

   private:
    std::ostream& out_;
    std::size_t bytes_ = 0;
    bool quiet_ = false;
    bool last_was_newline_ = true;
};

// ---------------------------------------------------------------------------
// Session restore — pick a session to resume.  Resolves `--resume ID` and
// `--continue NAME` against the session store.  Returns the resolved
// session id or empty string if none was requested / found.
// ---------------------------------------------------------------------------
std::string resolve_session_to_restore(const InputRequest& req);

}  // namespace hermes::cli::pipe_mode
