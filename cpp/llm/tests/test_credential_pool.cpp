#include "hermes/llm/credential_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using hermes::llm::CredentialPool;
using hermes::llm::PooledCredential;

namespace {

PooledCredential make_cred(std::string key, std::chrono::seconds ttl = 0s) {
    PooledCredential c;
    c.api_key = std::move(key);
    c.base_url = "https://example.invalid";
    if (ttl.count() > 0) {
        c.expires_at = std::chrono::system_clock::now() + ttl;
    }
    c.source = "test";
    return c;
}

}  // namespace

TEST(CredentialPool, StoreAndGetRoundTrip) {
    CredentialPool pool;
    pool.store("anthropic", make_cred("ak-test"));
    auto got = pool.get("anthropic");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->api_key, "ak-test");
    EXPECT_EQ(got->base_url, "https://example.invalid");
}

TEST(CredentialPool, GetReturnsNulloptWhenMissing) {
    CredentialPool pool;
    EXPECT_FALSE(pool.get("anthropic").has_value());
}

TEST(CredentialPool, ExpiredEntryTriggersRefresher) {
    CredentialPool pool;
    // Pre-populate with an expired entry.
    PooledCredential expired;
    expired.api_key = "stale";
    expired.expires_at = std::chrono::system_clock::now() - 10s;
    pool.store("openai", expired);

    std::atomic<int> calls{0};
    pool.set_refresher("openai", [&](const std::string& provider) {
        ++calls;
        EXPECT_EQ(provider, "openai");
        return make_cred("fresh-key", 60s);
    });

    auto got = pool.get("openai");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->api_key, "fresh-key");
    EXPECT_EQ(calls.load(), 1);

    // Second call within TTL should NOT hit refresher again.
    pool.get("openai");
    EXPECT_EQ(calls.load(), 1);
}

TEST(CredentialPool, EvictExpiredRemovesStale) {
    CredentialPool pool;
    // Three entries, one expired, one valid forever, one valid with TTL.
    PooledCredential stale;
    stale.api_key = "x";
    stale.expires_at = std::chrono::system_clock::now() - 5s;
    pool.store("stale", stale);
    pool.store("forever", make_cred("y"));
    pool.store("ttl", make_cred("z", 60s));

    EXPECT_EQ(pool.size(), 3u);
    auto evicted = pool.evict_expired();
    EXPECT_EQ(evicted, 1u);
    EXPECT_EQ(pool.size(), 2u);

    EXPECT_FALSE(pool.get("stale").has_value());
    EXPECT_TRUE(pool.get("forever").has_value());
    EXPECT_TRUE(pool.get("ttl").has_value());
}

TEST(CredentialPool, RefresherReturningNulloptClearsSlot) {
    CredentialPool pool;
    PooledCredential expired;
    expired.api_key = "stale";
    expired.expires_at = std::chrono::system_clock::now() - 1s;
    pool.store("qwen-oauth", expired);

    pool.set_refresher("qwen-oauth",
                       [](const std::string&) { return std::nullopt; });

    EXPECT_FALSE(pool.get("qwen-oauth").has_value());
    // And the stale slot is gone.
    EXPECT_EQ(pool.size(), 0u);
}

TEST(CredentialPool, InvalidateAndClear) {
    CredentialPool pool;
    pool.store("a", make_cred("1"));
    pool.store("b", make_cred("2"));
    pool.invalidate("a");
    EXPECT_FALSE(pool.get("a").has_value());
    EXPECT_TRUE(pool.get("b").has_value());
    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
}

TEST(CredentialPool, ThreadSafetyStressSmoke) {
    CredentialPool pool;
    pool.store("openai", make_cred("shared"));
    std::atomic<int> hits{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < 200; ++j) {
                if (auto c = pool.get("openai"); c && c->api_key == "shared") {
                    ++hits;
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    EXPECT_EQ(hits.load(), 8 * 200);
}

TEST(PooledCredential, IsExpiredAndIsUsable) {
    PooledCredential c;
    c.api_key = "k";
    EXPECT_FALSE(c.is_expired());  // default = never
    EXPECT_TRUE(c.is_usable());

    c.expires_at = std::chrono::system_clock::now() - 1s;
    EXPECT_TRUE(c.is_expired());
    EXPECT_FALSE(c.is_usable());

    c.expires_at = std::chrono::system_clock::now() + 60s;
    EXPECT_FALSE(c.is_expired());
    EXPECT_TRUE(c.is_usable());

    c.api_key.clear();
    EXPECT_FALSE(c.is_usable());  // empty key = unusable
}
