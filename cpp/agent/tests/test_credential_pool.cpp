#include "hermes/agent/credential_pool.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::creds;

namespace {

PoolEntry mk(const std::string& id, std::int64_t uses = 0,
             std::int64_t last_used = 0) {
    PoolEntry e;
    e.id = id;
    e.use_count = uses;
    e.last_used_sec = last_used;
    return e;
}

}  // namespace

TEST(CredentialPool, StrategyRoundTrip) {
    for (auto s : {Strategy::FillFirst, Strategy::RoundRobin,
                   Strategy::Random, Strategy::LeastUsed}) {
        EXPECT_EQ(parse_strategy(strategy_name(s)).value(), s);
    }
    EXPECT_FALSE(parse_strategy("not-a-strategy").has_value());
}

TEST(CredentialPool, PoolKeyCustom) {
    EXPECT_EQ(pool_key("openai"), "openai");
    EXPECT_EQ(pool_key("custom", "deepseek"), "custom:deepseek");
}

TEST(CredentialPool, FilterExhausted) {
    std::vector<PoolEntry> entries = {mk("a"), mk("b"), mk("c")};
    entries[1] = mark_exhausted(entries[1], /*now=*/1000);
    auto avail = filter_available(entries, /*now=*/1500);
    EXPECT_EQ(avail.size(), 2u);
    auto avail_after = filter_available(entries, /*now=*/10000);
    EXPECT_EQ(avail_after.size(), 3u);  // exhausted has expired (1000+3600)
}

TEST(CredentialPool, ChooseFillFirst) {
    std::vector<PoolEntry> entries = {mk("a"), mk("b")};
    auto c = choose_next(entries, Strategy::FillFirst, 0);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->id, "a");
}

TEST(CredentialPool, ChooseRoundRobinRotates) {
    std::vector<PoolEntry> entries = {mk("a"), mk("b"), mk("c")};
    EXPECT_EQ(choose_next(entries, Strategy::RoundRobin, 0, 0).value().id, "a");
    EXPECT_EQ(choose_next(entries, Strategy::RoundRobin, 0, 1).value().id, "b");
    EXPECT_EQ(choose_next(entries, Strategy::RoundRobin, 0, 2).value().id, "c");
    EXPECT_EQ(choose_next(entries, Strategy::RoundRobin, 0, 3).value().id, "a");
}

TEST(CredentialPool, ChooseLeastUsedBreaksTiesByLastUsed) {
    std::vector<PoolEntry> entries = {
        mk("a", 10, 1000),
        mk("b", 1, 2000),
        mk("c", 1, 500),
    };
    auto c = choose_next(entries, Strategy::LeastUsed, 0);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->id, "c");  // lowest use_count AND oldest last_used
}

TEST(CredentialPool, ChooseRandomWithinRange) {
    std::vector<PoolEntry> entries = {mk("a"), mk("b"), mk("c")};
    auto c = choose_next(entries, Strategy::Random, 0, 0, /*seed=*/42);
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(c->id == "a" || c->id == "b" || c->id == "c");
}

TEST(CredentialPool, NoAvailableReturnsNullopt) {
    std::vector<PoolEntry> entries = {mk("a")};
    entries[0] = mark_exhausted(entries[0], 0);
    auto c = choose_next(entries, Strategy::FillFirst, 100);
    EXPECT_FALSE(c.has_value());
}

TEST(CredentialPool, MarkExhaustedDefaultTtl) {
    PoolEntry e = mk("a");
    auto marked = mark_exhausted(e, /*now=*/100);
    EXPECT_EQ(marked.status, status::kExhausted);
    EXPECT_EQ(marked.exhausted_until_sec, 100 + kExhaustedTtlDefaultSeconds);
}

TEST(CredentialPool, MarkExhaustedExplicitUntil) {
    PoolEntry e = mk("a");
    auto marked = mark_exhausted(e, 100, /*until=*/500);
    EXPECT_EQ(marked.exhausted_until_sec, 500);
}
