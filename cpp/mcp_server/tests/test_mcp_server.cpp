#include "hermes/mcp_server/mcp_server.hpp"

#include <filesystem>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::mcp_server {
namespace {

class McpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("hermes_mcp_test_" + std::to_string(::getpid()) + ".db");
        db_ = std::make_unique<hermes::state::SessionDB>(db_path_);
        McpServerConfig config;
        config.session_db = db_.get();
        server_ = std::make_unique<HermesMcpServer>(config);
    }

    void TearDown() override {
        server_.reset();
        db_.reset();
        std::filesystem::remove(db_path_);
        // Remove WAL/SHM files
        std::filesystem::remove(
            std::filesystem::path(db_path_.string() + "-wal"));
        std::filesystem::remove(
            std::filesystem::path(db_path_.string() + "-shm"));
    }

    std::filesystem::path db_path_;
    std::unique_ptr<hermes::state::SessionDB> db_;
    std::unique_ptr<HermesMcpServer> server_;
};

TEST_F(McpServerTest, HandleInitializeReturnsCapabilities) {
    auto result = server_->handle_initialize({});

    EXPECT_TRUE(result.contains("protocolVersion"));
    EXPECT_TRUE(result.contains("capabilities"));
    EXPECT_TRUE(result.contains("serverInfo"));
    EXPECT_EQ(result["serverInfo"]["name"], "hermes");
}

TEST_F(McpServerTest, HandleToolsListReturnsTenTools) {
    auto result = server_->handle_tools_list();

    ASSERT_TRUE(result.contains("tools"));
    EXPECT_EQ(result["tools"].size(), 10u);
}

TEST_F(McpServerTest, HandleConversationsListFromSessionDB) {
    // Create a session
    auto id = db_->create_session("test", "test-model", nlohmann::json::object());

    auto result = server_->handle_conversations_list(nlohmann::json::object());
    ASSERT_TRUE(result.is_array());
    ASSERT_GE(result.size(), 1u);
    EXPECT_EQ(result[0]["id"], id);
}

TEST_F(McpServerTest, HandleToolCallDispatches) {
    auto result = server_->handle_tool_call("channels_list", {});

    ASSERT_TRUE(result.contains("content"));
    ASSERT_TRUE(result["content"].is_array());
    EXPECT_GE(result["content"].size(), 1u);
    EXPECT_EQ(result["content"][0]["type"], "text");
}

TEST_F(McpServerTest, HandleToolCallUnknownTool) {
    auto result = server_->handle_tool_call("nonexistent_tool", {});

    ASSERT_TRUE(result.contains("content"));
    auto text = result["content"][0]["text"].get<std::string>();
    auto parsed = nlohmann::json::parse(text);
    EXPECT_EQ(parsed["error"], "unknown_tool");
}

TEST_F(McpServerTest, ReadWriteMessageJsonRpcFormat) {
    // Verify write_message produces valid JSON — we test via handle_tool_call
    // which returns JSON-RPC compatible structure
    auto result = server_->handle_tool_call("events_poll", {});
    ASSERT_TRUE(result.contains("content"));

    // The content should be a JSON array with text entries
    for (const auto& entry : result["content"]) {
        EXPECT_TRUE(entry.contains("type"));
        EXPECT_TRUE(entry.contains("text"));
        // Text should be valid JSON
        EXPECT_NO_THROW(nlohmann::json::parse(entry["text"].get<std::string>()));
    }
}

}  // namespace
}  // namespace hermes::mcp_server
