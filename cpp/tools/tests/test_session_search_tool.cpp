#include "hermes/tools/session_search_tool.hpp"
#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>

using namespace hermes::tools;
namespace fs = std::filesystem;

namespace {

class SessionSearchToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_search_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        register_session_search_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch("session_search", args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SessionSearchToolTest, SearchReturnsResults) {
    hermes::state::SessionDB db;
    auto sid = db.create_session("cli", "gpt-4", {});
    hermes::state::MessageRow msg;
    msg.session_id = sid;
    msg.turn_index = 0;
    msg.role = "user";
    msg.content = "How do I configure the frobnicator?";
    db.save_message(msg);

    auto r = nlohmann::json::parse(dispatch({{"query", "frobnicator"}}));
    EXPECT_TRUE(r.contains("results"));
    EXPECT_TRUE(r.contains("count"));
    EXPECT_GE(r["count"].get<int>(), 1);
}

TEST_F(SessionSearchToolTest, EmptyQueryReturnsRecentMode) {
    auto r = nlohmann::json::parse(dispatch({{"query", ""}}));
    // empty query now yields the "recent" mode payload.
    EXPECT_EQ(r["mode"].get<std::string>(), "recent");
    EXPECT_EQ(r["count"].get<int>(), 0);
}

TEST_F(SessionSearchToolTest, NoQueryReturnsRecentMode) {
    auto r = nlohmann::json::parse(dispatch(nlohmann::json::object()));
    EXPECT_EQ(r["mode"].get<std::string>(), "recent");
}

TEST_F(SessionSearchToolTest, LimitClampedToFive) {
    auto r = nlohmann::json::parse(
        dispatch({{"query", "anything"}, {"limit", 99}}));
    EXPECT_LE(r["count"].get<int>(), 5);
}

TEST_F(SessionSearchToolTest, RoleFilterAccepted) {
    hermes::state::SessionDB db;
    auto sid = db.create_session("cli", "gpt-4", {});
    hermes::state::MessageRow m;
    m.session_id = sid;
    m.turn_index = 0;
    m.role = "user";
    m.content = "deploying to railway";
    db.save_message(m);

    auto r = nlohmann::json::parse(
        dispatch({{"query", "railway"}, {"role_filter", "user"}}));
    EXPECT_GE(r["count"].get<int>(), 1);
}

// ----- helper-level tests --------------------------------------------------

TEST(SessionSearchHelpers, FormatTimestampNullIsUnknown) {
    EXPECT_EQ(format_timestamp_human(nullptr), "unknown");
}

TEST(SessionSearchHelpers, FormatTimestampEmptyStringIsUnknown) {
    EXPECT_EQ(format_timestamp_human(std::string("")), "unknown");
}

TEST(SessionSearchHelpers, FormatTimestampNumeric) {
    auto out = format_timestamp_human(1700000000);
    EXPECT_NE(out.find("2023"), std::string::npos);
}

TEST(SessionSearchHelpers, FormatTimestampNumericString) {
    auto out = format_timestamp_human(std::string("1700000000"));
    EXPECT_NE(out.find("2023"), std::string::npos);
}

TEST(SessionSearchHelpers, FormatTimestampNonNumericString) {
    auto out = format_timestamp_human(std::string("not a date"));
    EXPECT_EQ(out, "not a date");
}

TEST(SessionSearchHelpers, ParseRoleFilterEmpty) {
    EXPECT_TRUE(parse_role_filter("").empty());
    EXPECT_TRUE(parse_role_filter("   ").empty());
}

TEST(SessionSearchHelpers, ParseRoleFilterCommaSeparated) {
    auto v = parse_role_filter("user, assistant ,tool");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "user");
    EXPECT_EQ(v[1], "assistant");
    EXPECT_EQ(v[2], "tool");
}

TEST(SessionSearchHelpers, IsHiddenSourceTrueForTool) {
    EXPECT_TRUE(is_hidden_source("tool"));
}

TEST(SessionSearchHelpers, IsHiddenSourceFalseForCli) {
    EXPECT_FALSE(is_hidden_source("cli"));
}

TEST(SessionSearchHelpers, TruncateAroundMatchesShortText) {
    std::string text(100, 'a');
    EXPECT_EQ(truncate_around_matches(text, "a", 200), text);
}

TEST(SessionSearchHelpers, TruncateAroundMatchesCentersOnHit) {
    // 1000 'a' chars + "needle" + 1000 'b' chars
    std::string head(1000, 'a');
    std::string tail(1000, 'b');
    std::string text = head + "needle" + tail;
    auto trimmed = truncate_around_matches(text, "needle", 200);
    EXPECT_NE(trimmed.find("needle"), std::string::npos);
    EXPECT_LT(trimmed.size(), 500u);
}

TEST(SessionSearchHelpers, TruncateAroundMatchesMissingTermFallsBack) {
    std::string text(2000, 'a');
    auto trimmed = truncate_around_matches(text, "xyz", 200);
    // size is 200 + maybe suffix marker
    EXPECT_GT(trimmed.size(), 200u);
    EXPECT_LT(trimmed.size(), 300u);
}

TEST(SessionSearchHelpers, FormatConversationUserAssistant) {
    std::vector<hermes::state::MessageRow> msgs;
    hermes::state::MessageRow u;
    u.role = "user";
    u.content = "hi";
    msgs.push_back(u);
    hermes::state::MessageRow a;
    a.role = "assistant";
    a.content = "hello";
    msgs.push_back(a);
    auto s = format_conversation(msgs);
    EXPECT_NE(s.find("[USER]: hi"), std::string::npos);
    EXPECT_NE(s.find("[ASSISTANT]: hello"), std::string::npos);
}

TEST(SessionSearchHelpers, FormatConversationToolCallAnnotations) {
    std::vector<hermes::state::MessageRow> msgs;
    hermes::state::MessageRow a;
    a.role = "assistant";
    a.content = "";
    a.tool_calls = nlohmann::json::array(
        {nlohmann::json{{"name", "shell"}},
         nlohmann::json{{"function", {{"name", "search"}}}}});
    msgs.push_back(a);
    auto s = format_conversation(msgs);
    EXPECT_NE(s.find("Called: shell, search"), std::string::npos);
}

TEST(SessionSearchHelpers, FormatConversationTruncatesLongTool) {
    std::vector<hermes::state::MessageRow> msgs;
    hermes::state::MessageRow t;
    t.role = "tool";
    t.content = std::string(2000, 'x');
    msgs.push_back(t);
    auto s = format_conversation(msgs);
    EXPECT_NE(s.find("[truncated]"), std::string::npos);
    EXPECT_LT(s.size(), 1200u);
}

TEST(SessionSearchHelpers, ResolveSessionRootWalksParents) {
    auto loader = [](const std::string& sid) -> nlohmann::json {
        if (sid == "child") {
            return {{"ok", true}, {"parent_session_id", "mid"}};
        }
        if (sid == "mid") {
            return {{"ok", true}, {"parent_session_id", "root"}};
        }
        if (sid == "root") {
            return {{"ok", true}};
        }
        return {{"ok", false}};
    };
    EXPECT_EQ(resolve_session_root("child", loader), "root");
}

TEST(SessionSearchHelpers, ResolveSessionRootHandlesMissing) {
    auto loader = [](const std::string&) -> nlohmann::json {
        return {{"ok", false}};
    };
    EXPECT_EQ(resolve_session_root("orphan", loader), "orphan");
}

TEST(SessionSearchHelpers, ResolveSessionRootBreaksLoops) {
    auto loader = [](const std::string& sid) -> nlohmann::json {
        if (sid == "a") return {{"ok", true}, {"parent_session_id", "b"}};
        if (sid == "b") return {{"ok", true}, {"parent_session_id", "a"}};
        return {{"ok", false}};
    };
    auto res = resolve_session_root("a", loader);
    EXPECT_TRUE(res == "a" || res == "b");
}

TEST(SessionSearchHelpers, FormatRecentSessionEntryShape) {
    hermes::state::SessionRow s;
    s.id = "abc";
    s.source = "cli";
    s.title = std::string("Hello");
    s.created_at = std::chrono::system_clock::now();
    s.updated_at = s.created_at;
    auto j = format_recent_session_entry(s, "preview");
    EXPECT_EQ(j["session_id"].get<std::string>(), "abc");
    EXPECT_EQ(j["source"].get<std::string>(), "cli");
    EXPECT_EQ(j["preview"].get<std::string>(), "preview");
    EXPECT_EQ(j["title"].get<std::string>(), "Hello");
}

TEST(SessionSearchHelpers, FormatRecentSessionEntryNullTitle) {
    hermes::state::SessionRow s;
    s.id = "x";
    s.source = "gateway";
    s.created_at = std::chrono::system_clock::now();
    s.updated_at = s.created_at;
    auto j = format_recent_session_entry(s, "");
    EXPECT_TRUE(j["title"].is_null());
}

}  // namespace
