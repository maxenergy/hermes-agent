#include "hermes/config/loader.hpp"

#include <gtest/gtest.h>

TEST(ManagedSystem, DetectDoesNotCrash) {
    // We can't assert a specific value (depends on CI host), but the
    // function must return one of the enum values without throwing.
    const auto result = hermes::config::detect_managed_system();
    switch (result) {
        case hermes::config::ManagedSystem::None:
        case hermes::config::ManagedSystem::NixOS:
        case hermes::config::ManagedSystem::Homebrew:
        case hermes::config::ManagedSystem::Debian:
            SUCCEED();
            return;
    }
    FAIL() << "unexpected ManagedSystem enum value";
}
