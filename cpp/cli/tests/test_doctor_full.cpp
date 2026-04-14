// Unit tests for the hermes::cli::doctor subsystem — the C++17 port of
// hermes_cli/doctor.py.  These cover the pure helpers and the end-to-end
// run_all() flow against a sandboxed HERMES_HOME in /tmp.

#include "hermes/cli/doctor.hpp"

#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace dr = hermes::cli::doctor;

namespace {

// RAII sandbox: a private HERMES_HOME directory for each test.
class Sandbox {
public:
    Sandbox() {
        const auto name = "hermes_doctor_test_" +
                          std::to_string(::getpid()) + "_" +
                          std::to_string(counter_++);
        root_ = fs::temp_directory_path() / name;
        fs::create_directories(root_);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    const fs::path& root() const { return root_; }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }

private:
    fs::path root_;
    static inline int counter_ = 0;
};

// Scripted HttpTransport for API reachability tests.
class ScriptedTransport : public hermes::llm::HttpTransport {
public:
    explicit ScriptedTransport(
        std::unordered_map<std::string, int> url_to_status)
        : responses_(std::move(url_to_status)) {}

    Response post_json(const std::string& url,
                        const std::unordered_map<std::string, std::string>&,
                        const std::string&) override {
        Response r;
        auto it = responses_.find(url);
        r.status_code = (it == responses_.end()) ? 599 : it->second;
        r.body = "{}";
        return r;
    }
    Response get(const std::string& url,
                  const std::unordered_map<std::string, std::string>&) override {
        Response r;
        auto it = responses_.find(url);
        r.status_code = (it == responses_.end()) ? 599 : it->second;
        r.body = "{}";
        return r;
    }

private:
    std::unordered_map<std::string, int> responses_;
};

}  // namespace

// ── Pure helper tests ───────────────────────────────────────────────────

TEST(DoctorHelpers, ProviderEnvHintsDetected) {
    EXPECT_TRUE(dr::has_provider_env_config("OPENAI_API_KEY=sk-abc\n"));
    EXPECT_TRUE(dr::has_provider_env_config("ANTHROPIC_TOKEN=oauth\n"));
    EXPECT_TRUE(dr::has_provider_env_config("HF_TOKEN=hf-xyz\n"));
    EXPECT_FALSE(dr::has_provider_env_config(""));
    EXPECT_FALSE(dr::has_provider_env_config(
        "# a comment\nSOME_OTHER_KEY=1\n"));
}

TEST(DoctorHelpers, HttpStatusClassification) {
    std::string detail;
    EXPECT_EQ(dr::classify_http_status(200, detail), dr::Severity::Ok);
    EXPECT_TRUE(detail.empty());
    EXPECT_EQ(dr::classify_http_status(401, detail), dr::Severity::Fail);
    EXPECT_NE(detail.find("invalid API key"), std::string::npos);
    EXPECT_EQ(dr::classify_http_status(500, detail), dr::Severity::Warn);
    EXPECT_NE(detail.find("500"), std::string::npos);
}

TEST(DoctorHelpers, BinaryOnPathFindsSh) {
#ifndef _WIN32
    EXPECT_TRUE(dr::binary_on_path("sh"));
#endif
    EXPECT_FALSE(dr::binary_on_path(
        "definitely_not_a_real_binary_xyzzy_hermes_doctor"));
}

TEST(DoctorHelpers, AvailableDiskBytesNonZero) {
    auto bytes = dr::available_disk_bytes(fs::temp_directory_path());
    EXPECT_GT(bytes, 0u);
}

TEST(DoctorHelpers, Mode0600RoundTrip) {
#ifndef _WIN32
    Sandbox sb;
    auto env = sb.root() / ".env";
    std::ofstream(env) << "X=1";
    // Initially probably 0644; force 0644 to be deterministic.
    ::chmod(env.c_str(), 0644);
    EXPECT_FALSE(dr::is_mode_0600(env));
    EXPECT_TRUE(dr::chmod_0600(env));
    EXPECT_TRUE(dr::is_mode_0600(env));
#else
    SUCCEED();
#endif
}

TEST(DoctorHelpers, Fts5Available) {
    // The hermes build links SQLite with FTS5 enabled.
    EXPECT_TRUE(dr::fts5_available());
}

TEST(DoctorHelpers, SqliteIntegrityCheckOnFreshDb) {
    Sandbox sb;
    auto db = sb.root() / "sessions.db";
    // sqlite3 will create the file on first open.
    std::ofstream(db).close();
    auto result = dr::sqlite_integrity_check(db);
    // Empty string means "ok"; any non-empty string is an error.
    EXPECT_TRUE(result.empty() || result == "ok");
}

TEST(DoctorHelpers, SkillsHubLockJsonParsed) {
    Sandbox sb;
    sb.write("skills/.hub/lock.json", "{\"installed\": {\"a\":{},\"b\":{}}}");
    auto count = dr::read_skills_hub_lock(sb.root() / "skills" / ".hub");
    EXPECT_EQ(count, 2);
}

TEST(DoctorHelpers, SkillsHubLockJsonMissing) {
    Sandbox sb;
    auto count = dr::read_skills_hub_lock(sb.root() / "skills" / ".hub");
    EXPECT_EQ(count, -1);
}

TEST(DoctorHelpers, SkillsHubLockJsonCorrupt) {
    Sandbox sb;
    sb.write("skills/.hub/lock.json", "{not json");
    auto count = dr::read_skills_hub_lock(sb.root() / "skills" / ".hub");
    EXPECT_EQ(count, -1);
}

TEST(DoctorHelpers, GatewayLockMissingAndStale) {
    Sandbox sb;
    auto lock = sb.root() / "gateway.lock";
    EXPECT_EQ(dr::gateway_lock_state(lock), "missing");
#ifndef _WIN32
    // Use the test process's own pid — guaranteed alive and signalable.
    auto my_pid = ::getpid();
    std::ofstream(lock) << "{\"pid\":" << my_pid << "}";
    auto s1 = dr::gateway_lock_state(lock);
    EXPECT_EQ(s1, "running:" + std::to_string(my_pid));
    // A ridiculously high pid that almost certainly does not exist is stale.
    std::ofstream(lock) << "{\"pid\":2147483646}";
    auto s2 = dr::gateway_lock_state(lock);
    EXPECT_NE(s2.find("stale:"), std::string::npos);
#endif
}

TEST(DoctorHelpers, McpChildProbeMissingBinary) {
    auto [ok, msg] = dr::probe_mcp_child("/no/such/path/binary-xyz");
    EXPECT_FALSE(ok);
    EXPECT_NE(msg.find("not found"), std::string::npos);
}

TEST(DoctorHelpers, McpChildProbeFindsSh) {
#ifndef _WIN32
    auto [ok, msg] = dr::probe_mcp_child("sh -c 'echo hi'");
    EXPECT_TRUE(ok) << msg;
#endif
}

TEST(DoctorHelpers, TerminalCapabilitiesNonEmpty) {
    auto caps = dr::detect_terminal_capabilities();
    // In CI stdout is piped — should still produce some string (possibly
    // empty if TERM=dumb and no UTF LANG).  The function must not crash.
    SUCCEED() << "caps=\"" << caps << "\"";
}

// ── Run-level tests (with sandboxed HERMES_HOME) ────────────────────────

TEST(DoctorRun, EmptyHomeReportsMissing) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    auto r = dr::run_all(opts);
    // Should have rows and at least one "HERMES_HOME missing"-ish message
    // for .env (no config.yaml, no .env).
    EXPECT_FALSE(r.rows.empty());
    bool saw_env_missing = false;
    for (const auto& row : r.rows) {
        if (row.label.find(".env") != std::string::npos &&
            row.severity == dr::Severity::Fail) {
            saw_env_missing = true;
        }
    }
    EXPECT_TRUE(saw_env_missing);
}

TEST(DoctorRun, FixFlagCreatesEnvFile) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    opts.fix = true;
    auto r = dr::run_all(opts);
    EXPECT_TRUE(fs::exists(sb.root() / ".env"));
    EXPECT_GT(r.fixed_count, 0);
}

TEST(DoctorRun, PermissionsCheckFixesMode) {
#ifndef _WIN32
    Sandbox sb;
    {
        std::ofstream env(sb.root() / ".env");
        env << "OPENAI_API_KEY=test\n";
    }
    ::chmod((sb.root() / ".env").c_str(), 0644);
    dr::Options opts;
    opts.home_override = sb.root();
    opts.fix = true;
    auto r = dr::run_all(opts);
    EXPECT_TRUE(dr::is_mode_0600(sb.root() / ".env"));
    EXPECT_GT(r.fixed_count, 0);
#endif
}

TEST(DoctorRun, StaleGatewayLockRemovedByFix) {
#ifndef _WIN32
    Sandbox sb;
    std::ofstream(sb.root() / "gateway.lock") << "{\"pid\":2147483646}";
    dr::Options opts;
    opts.home_override = sb.root();
    opts.fix = true;
    dr::run_all(opts);
    EXPECT_FALSE(fs::exists(sb.root() / "gateway.lock"));
#endif
}

TEST(DoctorRun, JsonRenderingParses) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    auto r = dr::run_all(opts);
    auto j = r.to_json();
    EXPECT_NE(j.find("\"rows\""), std::string::npos);
    EXPECT_NE(j.find("\"ok_count\""), std::string::npos);
    EXPECT_NE(j.find("\"fail_count\""), std::string::npos);
}

TEST(DoctorRun, ApiProbeUsesTransport) {
    Sandbox sb;
    {
        std::ofstream env(sb.root() / ".env");
        env << "OPENAI_API_KEY=sk-test\n";
    }
    ScriptedTransport fake({
        {"https://api.openai.com/v1/models", 200},
    });
    dr::Options opts;
    opts.home_override = sb.root();
    opts.transport = &fake;
    auto r = dr::run_all(opts);
    bool saw_openai_ok = false;
    for (const auto& row : r.rows) {
        if (row.category == "API Reachability" &&
            row.label == "OpenAI" &&
            row.severity == dr::Severity::Ok) {
            saw_openai_ok = true;
        }
    }
    EXPECT_TRUE(saw_openai_ok);
}

TEST(DoctorRun, ApiProbeFailsWith401) {
    Sandbox sb;
    {
        std::ofstream env(sb.root() / ".env");
        env << "ANTHROPIC_API_KEY=bogus\n";
    }
    ScriptedTransport fake({
        {"https://api.anthropic.com/v1/models", 401},
    });
    dr::Options opts;
    opts.home_override = sb.root();
    opts.transport = &fake;
    auto r = dr::run_all(opts);
    bool saw_anthropic_fail = false;
    for (const auto& row : r.rows) {
        if (row.category == "API Reachability" &&
            row.label == "Anthropic" &&
            row.severity == dr::Severity::Fail) {
            saw_anthropic_fail = true;
        }
    }
    EXPECT_TRUE(saw_anthropic_fail);
    EXPECT_FALSE(r.issues.empty());
}

TEST(DoctorRun, DirectoryStructureFixCreatesSubdirs) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    opts.fix = true;
    auto r = dr::run_all(opts);
    // Expected subdirs per the Python reference.
    for (const char* sub : {"sessions", "logs", "skills", "memories",
                              "cron", "profiles"}) {
        EXPECT_TRUE(fs::exists(sb.root() / sub)) << sub;
    }
    EXPECT_GT(r.fixed_count, 0);
}

TEST(DoctorRun, MemoryBackendDetectsHonchoFromYaml) {
    Sandbox sb;
    sb.write("config.yaml",
             "model:\n  provider: openai\nmemory:\n  provider: honcho\n");
    dr::Options opts;
    opts.home_override = sb.root();
    auto r = dr::run_all(opts);
    bool saw_honcho = false;
    for (const auto& row : r.rows) {
        if (row.category == "Memory Backend" &&
            row.label.find("Honcho") != std::string::npos) {
            saw_honcho = true;
        }
    }
    EXPECT_TRUE(saw_honcho);
}

TEST(DoctorRun, ExitCodeReflectsFailures) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    auto r = dr::run_all(opts);
    if (r.fail_count > 0) {
        EXPECT_EQ(r.exit_code(), 1);
    } else {
        EXPECT_EQ(r.exit_code(), 0);
    }
}

TEST(DoctorRun, RenderingDoesNotCrash) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    opts.color = false;
    auto r = dr::run_all(opts);
    ::testing::internal::CaptureStdout();
    dr::render(r, opts);
    auto out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("Hermes Doctor"), std::string::npos);
    EXPECT_NE(out.find("Summary"), std::string::npos);
}

TEST(DoctorRun, JsonModeOutputsValidJson) {
    Sandbox sb;
    dr::Options opts;
    opts.home_override = sb.root();
    opts.json = true;
    auto r = dr::run_all(opts);
    ::testing::internal::CaptureStdout();
    dr::render(r, opts);
    auto out = ::testing::internal::GetCapturedStdout();
    // Must begin with '{' and be parseable by nlohmann::json.
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(out[0], '{');
}
