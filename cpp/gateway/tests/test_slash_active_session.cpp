// Tests for classify_for_active_session — ports upstream 632a807a behaviour.
//
// Rule: while a session's agent is running, every recognized slash
// command bypasses the interrupt path.  Dedicated handlers (/stop,
// /help, /status, /reset, /approve, /deny …) run in place; other
// commands get a graceful "agent busy" reply without interrupting.
// /queue bypasses entirely, queuing the prompt for the next turn.
// Plain text or unknown slash tokens fall through to the runner's
// normal busy-session queue path.
#include <gtest/gtest.h>

#include <hermes/gateway/command_dispatcher.hpp>

namespace hg = hermes::gateway;

class SlashActiveSessionTest : public ::testing::Test {
protected:
    hg::CommandDispatcher d_;
    void SetUp() override { d_.register_builtins(); }
};

TEST_F(SlashActiveSessionTest, PlainTextFallsThrough) {
    auto dec = d_.classify_for_active_session("hello, world");
    EXPECT_EQ(dec.routing, hg::ActiveSessionRouting::NotACommand);
    EXPECT_TRUE(dec.canonical.empty());
}

TEST_F(SlashActiveSessionTest, UnknownSlashCommandFallsThrough) {
    auto dec = d_.classify_for_active_session("/bogus_unknown_cmd");
    EXPECT_EQ(dec.routing, hg::ActiveSessionRouting::NotACommand);
}

TEST_F(SlashActiveSessionTest, DedicatedStopRunsHandler) {
    auto dec = d_.classify_for_active_session("/stop");
    EXPECT_EQ(dec.routing,
               hg::ActiveSessionRouting::BypassRunsDedicatedHandler);
    EXPECT_EQ(dec.canonical, "stop");
}

TEST_F(SlashActiveSessionTest, DedicatedAliasResolves) {
    // "cancel" / "abort" are aliases for "stop".
    auto dec1 = d_.classify_for_active_session("/cancel");
    EXPECT_EQ(dec1.routing,
               hg::ActiveSessionRouting::BypassRunsDedicatedHandler);
    EXPECT_EQ(dec1.canonical, "stop");
    auto dec2 = d_.classify_for_active_session("/abort now");
    EXPECT_EQ(dec2.routing,
               hg::ActiveSessionRouting::BypassRunsDedicatedHandler);
    EXPECT_EQ(dec2.canonical, "stop");
}

TEST_F(SlashActiveSessionTest, DedicatedHelpResetApproveDeny) {
    for (auto* cmd : {"/help", "/reset", "/new", "/approve", "/deny",
                        "/drain", "/replay", "/yes", "/no"}) {
        auto dec = d_.classify_for_active_session(cmd);
        EXPECT_EQ(dec.routing,
                   hg::ActiveSessionRouting::BypassRunsDedicatedHandler)
            << "command=" << cmd;
    }
}

TEST_F(SlashActiveSessionTest, NonDedicatedModelGracefullyRejected) {
    // /model is recognized but does NOT have a dedicated running-agent
    // handler — it must NOT interrupt the agent, so we reject with a
    // user-visible reply.
    auto dec = d_.classify_for_active_session("/model gpt-4o");
    EXPECT_EQ(dec.routing, hg::ActiveSessionRouting::BypassGracefulReject);
    EXPECT_EQ(dec.canonical, "model");
    EXPECT_FALSE(dec.reply.empty());
    // The reply names the command so the user knows what they tried.
    EXPECT_NE(dec.reply.find("/model"), std::string::npos);
    // And instructs them how to recover.
    EXPECT_NE(dec.reply.find("/stop"), std::string::npos);
}

TEST_F(SlashActiveSessionTest, NonDedicatedRestartShutdownRejected) {
    for (auto* cmd : {"/restart", "/shutdown", "/quit"}) {
        auto dec = d_.classify_for_active_session(cmd);
        EXPECT_EQ(dec.routing,
                   hg::ActiveSessionRouting::BypassGracefulReject)
            << "command=" << cmd;
    }
}

TEST_F(SlashActiveSessionTest, QueueCommandBypassesEntirely) {
    auto dec = d_.classify_for_active_session("/queue write a haiku");
    EXPECT_EQ(dec.routing, hg::ActiveSessionRouting::QueueBypass);
    EXPECT_EQ(dec.canonical, "queue");
}

TEST_F(SlashActiveSessionTest, DedicatedHandlerRegistrationQueryable) {
    EXPECT_TRUE(d_.has_dedicated_running_handler("stop"));
    EXPECT_TRUE(d_.has_dedicated_running_handler("help"));
    EXPECT_TRUE(d_.has_dedicated_running_handler("queue"));
    EXPECT_FALSE(d_.has_dedicated_running_handler("model"));
    EXPECT_FALSE(d_.has_dedicated_running_handler("restart"));
    // Unknown command.
    EXPECT_FALSE(d_.has_dedicated_running_handler("nonexistent"));
}

TEST_F(SlashActiveSessionTest, ReRegisterOverwritesDedicatedFlag) {
    hg::CommandDispatcher d;
    int call = 0;
    d.register_command("x",
                        [&](const hg::CommandContext&,
                            const std::string&) -> hg::DispatchResult {
                            ++call;
                            return {};
                        },
                        {}, /*dedicated=*/true);
    EXPECT_TRUE(d.has_dedicated_running_handler("x"));
    d.register_command("x",
                        [&](const hg::CommandContext&,
                            const std::string&) -> hg::DispatchResult {
                            ++call;
                            return {};
                        },
                        {}, /*dedicated=*/false);
    EXPECT_FALSE(d.has_dedicated_running_handler("x"));
}
