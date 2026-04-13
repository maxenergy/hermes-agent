// Argv pre-parse for `--profile` / `-p` — mirrors the Python
// `_apply_profile_override()` hook that runs before any module import.
//
// Each fixture simulates `main(argc, argv)` by building a heap-allocated
// argv vector with writable tokens, calls `preparse_profile_argv()`,
// then asserts (a) the extracted name and (b) the surviving argv slice.

#include "hermes/profile/profile.hpp"

#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hp = hermes::profile;

namespace {

// Owns the backing char buffers and hands out a writable char* argv.
class Argv {
public:
    explicit Argv(std::vector<std::string> toks) : toks_(std::move(toks)) {
        ptrs_.reserve(toks_.size() + 1);
        for (auto& t : toks_) {
            ptrs_.push_back(t.data());
        }
        ptrs_.push_back(nullptr);
        argc_ = static_cast<int>(toks_.size());
    }
    int& argc() { return argc_; }
    char** argv() { return ptrs_.data(); }

    std::vector<std::string> surviving() const {
        std::vector<std::string> out;
        for (int i = 0; i < argc_; ++i) {
            out.emplace_back(ptrs_[i] ? ptrs_[i] : "");
        }
        return out;
    }

private:
    std::vector<std::string> toks_;
    std::vector<char*> ptrs_;
    int argc_ = 0;
};

}  // namespace

TEST(PreparseProfileArgv, NoFlagReturnsNullopt) {
    Argv a({"hermes", "chat", "hello"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(a.argc(), 3);
    EXPECT_EQ(a.surviving()[1], "chat");
}

TEST(PreparseProfileArgv, DashPExtractsAndStrips) {
    Argv a({"hermes", "-p", "coder", "chat"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "coder");
    EXPECT_EQ(a.argc(), 2);
    auto surv = a.surviving();
    EXPECT_EQ(surv[0], "hermes");
    EXPECT_EQ(surv[1], "chat");
}

TEST(PreparseProfileArgv, LongProfileSpaceExtracts) {
    Argv a({"hermes", "--profile", "bot", "gateway"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "bot");
    EXPECT_EQ(a.argc(), 2);
    EXPECT_EQ(a.surviving()[1], "gateway");
}

TEST(PreparseProfileArgv, LongProfileEqualsExtracts) {
    Argv a({"hermes", "--profile=qa", "status"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "qa");
    EXPECT_EQ(a.argc(), 2);
    EXPECT_EQ(a.surviving()[1], "status");
}

TEST(PreparseProfileArgv, FlagInMiddleStrips) {
    Argv a({"hermes", "status", "-p", "dev", "--verbose"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "dev");
    EXPECT_EQ(a.argc(), 3);
    auto surv = a.surviving();
    EXPECT_EQ(surv[0], "hermes");
    EXPECT_EQ(surv[1], "status");
    EXPECT_EQ(surv[2], "--verbose");
}

TEST(PreparseProfileArgv, DanglingLongProfileIsNoop) {
    // Trailing --profile with no value — must be a no-op (leave argv alone).
    Argv a({"hermes", "--profile"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(a.argc(), 2);
}

TEST(PreparseProfileArgv, DanglingDashPIsNoop) {
    Argv a({"hermes", "-p"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(a.argc(), 2);
}

TEST(PreparseProfileArgv, StopsAtDoubleDash) {
    // --profile after `--` is the user's args, not a hermes flag.
    Argv a({"hermes", "chat", "--", "--profile", "ignored"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(a.argc(), 5);
    EXPECT_EQ(a.surviving()[3], "--profile");
}

TEST(PreparseProfileArgv, EmptyValueIgnored) {
    Argv a({"hermes", "--profile=", "chat"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    EXPECT_FALSE(got.has_value());
}

TEST(PreparseProfileArgv, ZeroArgcIsSafe) {
    int argc = 0;
    char* empty[] = {nullptr};
    auto got = hp::preparse_profile_argv(argc, empty);
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(argc, 0);
}

TEST(PreparseProfileArgv, FirstMatchWins) {
    // If two profile flags are present the first wins; the second stays
    // in argv (treated as user input).  This matches argparse semantics.
    Argv a({"hermes", "-p", "first", "-p", "second"});
    auto got = hp::preparse_profile_argv(a.argc(), a.argv());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "first");
    EXPECT_EQ(a.argc(), 3);
    auto surv = a.surviving();
    EXPECT_EQ(surv[1], "-p");
    EXPECT_EQ(surv[2], "second");
}
