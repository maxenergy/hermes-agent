#include "hermes/approval/gateway_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using hermes::approval::ApprovalDecision;
using hermes::approval::ApprovalRequest;
using hermes::approval::GatewayApprovalQueue;

TEST(GatewayQueue, EnqueueAndResolveApproved) {
    GatewayApprovalQueue queue;

    ApprovalRequest req;
    req.request_id = "req1";
    req.session_key = "sess1";
    req.command = "rm -rf /tmp/test";

    ApprovalDecision result = ApprovalDecision::Denied;

    std::thread waiter([&] {
        result = queue.enqueue_and_wait(
            req, [](const ApprovalRequest&) {}, std::chrono::seconds(5));
    });

    // Give the waiter thread a moment to enter the wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    queue.resolve("sess1", "req1", ApprovalDecision::Approved);
    waiter.join();

    EXPECT_EQ(result, ApprovalDecision::Approved);
}

TEST(GatewayQueue, TimeoutReturnsDenied) {
    GatewayApprovalQueue queue;

    ApprovalRequest req;
    req.request_id = "req2";
    req.session_key = "sess2";
    req.command = "dangerous";

    // Use a very short timeout.
    auto result = queue.enqueue_and_wait(
        req, [](const ApprovalRequest&) {}, std::chrono::seconds(0));

    EXPECT_EQ(result, ApprovalDecision::Denied);
}

TEST(GatewayQueue, CancelSessionWakesPending) {
    GatewayApprovalQueue queue;

    ApprovalRequest req;
    req.request_id = "req3";
    req.session_key = "sess3";
    req.command = "test";

    ApprovalDecision result = ApprovalDecision::Approved;

    std::thread waiter([&] {
        result = queue.enqueue_and_wait(
            req, [](const ApprovalRequest&) {}, std::chrono::seconds(5));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.cancel_session("sess3");
    waiter.join();

    EXPECT_EQ(result, ApprovalDecision::Denied);
}
