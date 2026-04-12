#include "hermes/tools/credential_files.hpp"

#include <gtest/gtest.h>

#include <string>

using hermes::tools::credential_path;
using hermes::tools::hermes_home;

TEST(CredentialFiles, CredentialPathUnderHermesHome) {
    auto cred = credential_path("test_key.json");
    auto home = hermes_home();
    auto cred_str = cred.string();
    auto home_str = home.string();
    EXPECT_NE(cred_str.find(home_str), std::string::npos);
}

TEST(CredentialFiles, HermesHomeNotEmpty) {
    auto home = hermes_home();
    EXPECT_FALSE(home.empty());
}
