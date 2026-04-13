// Tests for the gateway media cache (download + retry + SSRF).
#include <gtest/gtest.h>

#include <hermes/gateway/media_cache.hpp>
#include <hermes/llm/llm_client.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using hermes::gateway::MediaCache;
using hermes::gateway::MediaKind;
using hermes::llm::FakeHttpTransport;

namespace fs = std::filesystem;

namespace {

class MediaCacheEnv : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("hermes-mc-" + std::to_string(::getpid()) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmp_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    MediaCache::Options opts() const {
        MediaCache::Options o;
        o.root_override = tmp_;
        o.max_attempts = 3;
        o.base_delay = std::chrono::milliseconds(1);
        o.max_delay = std::chrono::milliseconds(2);
        o.ssrf_protection = false;  // tests use http://example.com
        return o;
    }

    fs::path tmp_;
};

}  // namespace

TEST_F(MediaCacheEnv, FetchesAndCachesImage) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "PNG_BYTES", {}});

    MediaCache mc(opts(), &fake);
    auto r = mc.fetch("https://cdn.example.com/foo.png", MediaKind::Image);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.from_cache);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_EQ(r.path.parent_path().filename(), "images");
    EXPECT_TRUE(r.path.string().find(".png") != std::string::npos);

    std::ifstream in(r.path, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "PNG_BYTES");
}

TEST_F(MediaCacheEnv, ServesFromCacheOnSecondFetch) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "AUDIO_BYTES", {}});

    MediaCache mc(opts(), &fake);
    auto r1 = mc.fetch("https://cdn.example.com/clip.ogg", MediaKind::Audio);
    ASSERT_TRUE(r1.ok);
    EXPECT_FALSE(r1.from_cache);

    auto r2 = mc.fetch("https://cdn.example.com/clip.ogg", MediaKind::Audio);
    ASSERT_TRUE(r2.ok);
    EXPECT_TRUE(r2.from_cache);
    EXPECT_EQ(fake.requests().size(), 1u);
}

TEST_F(MediaCacheEnv, RetriesOn429ThenSucceeds) {
    FakeHttpTransport fake;
    fake.enqueue_response({429, "rate limited", {}});
    fake.enqueue_response({503, "down", {}});
    fake.enqueue_response({200, "OK", {}});

    MediaCache mc(opts(), &fake);
    auto r = mc.fetch("https://cdn.example.com/doc.pdf", MediaKind::Document);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.attempts, 3);
    EXPECT_EQ(fake.requests().size(), 3u);
}

TEST_F(MediaCacheEnv, FailsFastOn404) {
    FakeHttpTransport fake;
    fake.enqueue_response({404, "missing", {}});

    MediaCache mc(opts(), &fake);
    auto r = mc.fetch("https://cdn.example.com/missing.png", MediaKind::Image);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.attempts, 1);
    EXPECT_EQ(r.status_code, 404);
}

TEST_F(MediaCacheEnv, ExhaustsRetriesOn5xx) {
    FakeHttpTransport fake;
    for (int i = 0; i < 5; ++i) {
        fake.enqueue_response({500, "boom", {}});
    }

    MediaCache mc(opts(), &fake);
    auto r = mc.fetch("https://cdn.example.com/big.bin", MediaKind::Document);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.attempts, 3);  // max_attempts from opts()
}

TEST_F(MediaCacheEnv, BlocksPrivateAddressWhenSsrfEnabled) {
    auto o = opts();
    o.ssrf_protection = true;
    MediaCache mc(o);
    auto r = mc.fetch("http://127.0.0.1:8080/x.png", MediaKind::Image);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ssrf"), std::string::npos);
}

TEST(MediaCacheClassify, RetryableStatuses) {
    EXPECT_TRUE(MediaCache::is_retryable_status(429));
    EXPECT_TRUE(MediaCache::is_retryable_status(500));
    EXPECT_TRUE(MediaCache::is_retryable_status(503));
    EXPECT_TRUE(MediaCache::is_retryable_status(408));
    EXPECT_FALSE(MediaCache::is_retryable_status(200));
    EXPECT_FALSE(MediaCache::is_retryable_status(404));
    EXPECT_FALSE(MediaCache::is_retryable_status(401));
}
