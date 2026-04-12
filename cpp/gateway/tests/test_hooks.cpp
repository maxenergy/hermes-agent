#include <gtest/gtest.h>

#include <hermes/gateway/hooks.hpp>

namespace hg = hermes::gateway;

TEST(HookRegistry, RegisterAndEmit) {
    hg::HookRegistry registry;
    int called = 0;
    std::string received_event;

    registry.register_hook("session:start",
                           [&](const std::string& evt, const nlohmann::json&) {
                               ++called;
                               received_event = evt;
                           });

    registry.emit("session:start");
    EXPECT_EQ(called, 1);
    EXPECT_EQ(received_event, "session:start");

    // Non-matching event should not fire.
    registry.emit("session:end");
    EXPECT_EQ(called, 1);
}

TEST(HookRegistry, WildcardCommandStar) {
    hg::HookRegistry registry;
    std::vector<std::string> events;

    registry.register_hook("command:*",
                           [&](const std::string& evt, const nlohmann::json&) {
                               events.push_back(evt);
                           });

    registry.emit("command:new");
    registry.emit("command:reset");
    registry.emit("session:start");  // Should NOT match.

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], "command:new");
    EXPECT_EQ(events[1], "command:reset");
}

TEST(HookRegistry, ExceptionDoesNotCrash) {
    hg::HookRegistry registry;
    int second_called = 0;

    registry.register_hook(
        "test:event",
        [](const std::string&, const nlohmann::json&) {
            throw std::runtime_error("boom");
        });

    registry.register_hook("test:event",
                           [&](const std::string&, const nlohmann::json&) {
                               ++second_called;
                           });

    // Should not throw, and second handler should still fire.
    EXPECT_NO_THROW(registry.emit("test:event"));
    EXPECT_EQ(second_called, 1);
}

TEST(HookRegistry, ClearAndSize) {
    hg::HookRegistry registry;

    EXPECT_EQ(registry.size(), 0u);

    registry.register_hook("a", [](const std::string&, const nlohmann::json&) {});
    registry.register_hook("b", [](const std::string&, const nlohmann::json&) {});
    EXPECT_EQ(registry.size(), 2u);

    registry.clear();
    EXPECT_EQ(registry.size(), 0u);

    // Emit after clear should do nothing.
    registry.emit("a");
}
