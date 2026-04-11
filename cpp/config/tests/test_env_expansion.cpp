#include "hermes/config/loader.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace hc = hermes::config;

namespace {
class ScopedEnv {
public:
    ScopedEnv(std::string key, const char* value) : key_(std::move(key)) {
        if (const char* old = std::getenv(key_.c_str()); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        if (value == nullptr) {
            ::unsetenv(key_.c_str());
        } else {
            ::setenv(key_.c_str(), value, 1);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(key_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string key_;
    bool had_old_ = false;
    std::string old_;
};
}  // namespace

TEST(ExpandEnvVars, SimpleBracedExpansion) {
    ScopedEnv guard("HERMES_TEST_VAR", "world");
    EXPECT_EQ(hc::expand_env_vars("hello-${HERMES_TEST_VAR}"), "hello-world");
}

TEST(ExpandEnvVars, FallbackWhenUnset) {
    ScopedEnv guard("HERMES_UNSET_XYZ", nullptr);
    EXPECT_EQ(hc::expand_env_vars("${HERMES_UNSET_XYZ:-fallback}"), "fallback");
}

TEST(ExpandEnvVars, FallbackEmptyString) {
    ScopedEnv guard("HERMES_UNSET_XYZ", nullptr);
    EXPECT_EQ(hc::expand_env_vars("${HERMES_UNSET_XYZ:-}"), "");
}

TEST(ExpandEnvVars, PresentBeatsFallback) {
    ScopedEnv guard("HERMES_TEST_VAR2", "real");
    EXPECT_EQ(hc::expand_env_vars("${HERMES_TEST_VAR2:-fallback}"), "real");
}

TEST(ExpandEnvVars, UnresolvedKeptVerbatim) {
    ScopedEnv guard("HERMES_NOPE_NOPE", nullptr);
    // Without a `:-`, unresolved variables remain as-is so callers can
    // detect missing values (matches the Python reference).
    EXPECT_EQ(hc::expand_env_vars("${HERMES_NOPE_NOPE}/tail"),
              "${HERMES_NOPE_NOPE}/tail");
}

TEST(ExpandEnvVars, DoubleDollarStaysLiteral) {
    // We only interpret `${...}`.  Bare `$$` and `$VAR` are literal.
    EXPECT_EQ(hc::expand_env_vars("price is $$5"), "price is $$5");
    EXPECT_EQ(hc::expand_env_vars("bare $HOME"), "bare $HOME");
}

TEST(ExpandEnvVars, HomeExpansion) {
    ScopedEnv guard("HOME", "/tmp/fake-home");
    EXPECT_EQ(hc::expand_env_vars("${HOME}/x"), "/tmp/fake-home/x");
}
