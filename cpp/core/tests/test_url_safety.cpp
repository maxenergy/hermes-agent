#include "hermes/core/url_safety.hpp"

#include <gtest/gtest.h>

namespace hus = hermes::core::url_safety;

TEST(UrlSafety, RejectsPrivateIpv4) {
    EXPECT_TRUE(hus::is_private_address("10.0.0.1"));
    EXPECT_TRUE(hus::is_private_address("172.16.0.1"));
    EXPECT_TRUE(hus::is_private_address("172.31.255.255"));
    EXPECT_TRUE(hus::is_private_address("192.168.1.1"));
    EXPECT_TRUE(hus::is_private_address("127.0.0.1"));
    EXPECT_TRUE(hus::is_private_address("169.254.169.254"));  // metadata
}

TEST(UrlSafety, AllowsPublicIpv4) {
    EXPECT_FALSE(hus::is_private_address("8.8.8.8"));
    EXPECT_FALSE(hus::is_private_address("172.32.0.1"));  // just outside 172.16/12
    EXPECT_FALSE(hus::is_private_address("192.169.1.1"));
}

TEST(UrlSafety, RejectsLocalhostAndMetadataNames) {
    EXPECT_TRUE(hus::is_private_address("localhost"));
    EXPECT_TRUE(hus::is_private_address("LOCALHOST"));
    EXPECT_TRUE(hus::is_private_address("metadata.google.internal"));
}

TEST(UrlSafety, RejectsIpv6Loopback) {
    EXPECT_TRUE(hus::is_private_address("::1"));
    EXPECT_TRUE(hus::is_private_address("fe80::1"));
    EXPECT_TRUE(hus::is_private_address("fd12:3456::1"));
    EXPECT_TRUE(hus::is_private_address("fc00::1"));
}

TEST(UrlSafety, IsSafeUrlHappyPath) {
    EXPECT_TRUE(hus::is_safe_url("https://api.openai.com/v1/chat"));
    EXPECT_FALSE(hus::is_safe_url("http://127.0.0.1:8080/admin"));
    EXPECT_FALSE(hus::is_safe_url("http://metadata.google.internal/latest"));
    EXPECT_FALSE(hus::is_safe_url("not a url"));
}

TEST(UrlSafety, HandlesIpv6BracketInUrl) {
    EXPECT_FALSE(hus::is_safe_url("http://[::1]:80/"));
    EXPECT_FALSE(hus::is_safe_url("https://[fe80::1]/path"));
}
