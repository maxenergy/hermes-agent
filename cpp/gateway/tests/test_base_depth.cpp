// Tests for gateway::base_depth — pure helpers ported from
// gateway/platforms/base.py.

#include <hermes/gateway/base_depth.hpp>

#include <array>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

namespace bd = hermes::gateway::base_depth;

namespace {

// A callable getenv that reads from a fixture table.
thread_local std::unordered_map<std::string, std::string> g_env_table;

std::string fake_getenv(const char* name) {
    const auto it = g_env_table.find(name ? name : "");
    return it == g_env_table.end() ? std::string{} : it->second;
}

}  // namespace

TEST(BaseDepthUtf16, AsciiMatchesCodepoints) {
    EXPECT_EQ(bd::utf16_len("hello"), 5u);
    EXPECT_EQ(bd::utf16_len(""), 0u);
}

TEST(BaseDepthUtf16, BmpCountsAsOne) {
    // Basic CJK: 3 bytes per codepoint but still 1 UTF-16 unit each.
    EXPECT_EQ(bd::utf16_len("\xe4\xb8\xad\xe6\x96\x87"), 2u);  // "中文"
}

TEST(BaseDepthUtf16, SurrogatePairCountsAsTwo) {
    // 😀 U+1F600 — outside BMP, surrogate pair.
    EXPECT_EQ(bd::utf16_len("\xF0\x9F\x98\x80"), 2u);
}

TEST(BaseDepthUtf16, PrefixBoundaryNeverSplitsSurrogate) {
    // Two emoji: each 2 UTF-16 units.
    const std::string s{"\xF0\x9F\x98\x80\xF0\x9F\x98\x80"};
    EXPECT_EQ(bd::prefix_within_utf16_limit(s, 4).size(), s.size());
    // Budget of 3 fits only the first emoji (2 units).
    const auto out = bd::prefix_within_utf16_limit(s, 3);
    EXPECT_EQ(out.size(), 4u);
}

TEST(BaseDepthSafeUrl, StripsUserinfoAndQuery) {
    EXPECT_EQ(bd::safe_url_for_log("https://u:p@example.com/path/file.png?x=1"),
              "https://example.com/.../file.png");
}

TEST(BaseDepthSafeUrl, BareHost) {
    EXPECT_EQ(bd::safe_url_for_log("https://example.com"), "https://example.com");
}

TEST(BaseDepthSafeUrl, ElidesLongUrl) {
    const std::string long_url{std::string{"https://example.com/a/"} +
                                 std::string(200, 'x')};
    const auto out = bd::safe_url_for_log(long_url, 40);
    EXPECT_LE(out.size(), 40u);
    EXPECT_EQ(out.substr(out.size() - 3), "...");
}

TEST(BaseDepthSafeUrl, MaxLenThreeOrLess) {
    EXPECT_EQ(bd::safe_url_for_log("https://x/y", 3), "...");
    EXPECT_EQ(bd::safe_url_for_log("https://x/y", 0), "");
}

TEST(BaseDepthResolveProxy, EnvPriorityOrder) {
    g_env_table.clear();
    g_env_table["HTTPS_PROXY"] = "http://1.1.1.1:8080";
    g_env_table["HTTP_PROXY"] = "http://2.2.2.2:8080";
    g_env_table["TELEGRAM_PROXY"] = "http://override:9090";
    auto r = bd::resolve_proxy_url("TELEGRAM_PROXY", &fake_getenv);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "http://override:9090");
    r = bd::resolve_proxy_url({}, &fake_getenv);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "http://1.1.1.1:8080");
    g_env_table.erase("HTTPS_PROXY");
    r = bd::resolve_proxy_url({}, &fake_getenv);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "http://2.2.2.2:8080");
    g_env_table.clear();
    r = bd::resolve_proxy_url({}, &fake_getenv);
    EXPECT_FALSE(r.has_value());
}

TEST(BaseDepthClassifyProxy, DetectsSocksAndHttp) {
    EXPECT_EQ(bd::classify_proxy("socks5://host:1080"), bd::ProxyKind::Socks);
    EXPECT_EQ(bd::classify_proxy("SOCKS5H://h"), bd::ProxyKind::Socks);
    EXPECT_EQ(bd::classify_proxy("http://h"), bd::ProxyKind::Http);
    EXPECT_EQ(bd::classify_proxy("https://h"), bd::ProxyKind::Http);
    EXPECT_EQ(bd::classify_proxy(""), bd::ProxyKind::None);
    EXPECT_EQ(bd::classify_proxy("ftp://h"), bd::ProxyKind::Unknown);
}

TEST(BaseDepthLooksLikeImage, Magics) {
    // Each magic signature must be checked with >= 4 bytes to mirror
    // the Python precondition (``if len(data) < 4: return False``).
    const std::string png{"\x89PNG\r\n\x1a\n", 8};
    const std::string jpg{"\xff\xd8\xff\xe0"};  // JPEG SOI + APP0
    const std::string gif{"GIF89a"};
    const std::string bmp{"BM\x00\x00", 4};
    EXPECT_TRUE(bd::looks_like_image(png));
    EXPECT_TRUE(bd::looks_like_image(jpg));
    EXPECT_TRUE(bd::looks_like_image(gif));
    EXPECT_TRUE(bd::looks_like_image(bmp));
    // Need 12 bytes for WEBP.
    std::string full_webp = std::string{"RIFF....WEBP"};
    EXPECT_TRUE(bd::looks_like_image(full_webp));
    EXPECT_FALSE(bd::looks_like_image("<html"));
    // Tiny inputs fail the precondition.
    EXPECT_FALSE(bd::looks_like_image(std::string{"BM"}));
}

TEST(BaseDepthIsAnimationUrl, StripsQueryAndChecksGif) {
    EXPECT_TRUE(bd::is_animation_url("https://host/path.gif"));
    EXPECT_TRUE(bd::is_animation_url("https://host/path.GIF?x=1"));
    EXPECT_FALSE(bd::is_animation_url("https://host/path.png"));
    EXPECT_FALSE(bd::is_animation_url(""));
}

TEST(BaseDepthExtractImages, MarkdownAndHtml) {
    const std::string content =
        "Hello ![alt](https://cdn/pic.png) world "
        "<img src=\"https://cdn/pic2.jpg\"> and "
        "![x](https://nope/other.txt) end";
    const auto [imgs, cleaned] = bd::extract_images(content);
    ASSERT_EQ(imgs.size(), 2u);
    EXPECT_EQ(imgs[0].first, "https://cdn/pic.png");
    EXPECT_EQ(imgs[0].second, "alt");
    EXPECT_EQ(imgs[1].first, "https://cdn/pic2.jpg");
    EXPECT_TRUE(cleaned.find("pic.png") == std::string::npos);
    EXPECT_TRUE(cleaned.find("pic2.jpg") == std::string::npos);
    // Non-image markdown is preserved.
    EXPECT_TRUE(cleaned.find("other.txt") != std::string::npos);
}

TEST(BaseDepthExtractMedia, VoiceTagAndPaths) {
    const std::string content =
        "some text\n[[audio_as_voice]]\nMEDIA:/tmp/x.ogg\ngoodbye";
    const auto [media, cleaned] = bd::extract_media(content);
    ASSERT_EQ(media.size(), 1u);
    EXPECT_EQ(media[0].first, "/tmp/x.ogg");
    EXPECT_TRUE(media[0].second);
    EXPECT_TRUE(cleaned.find("[[audio_as_voice]]") == std::string::npos);
    EXPECT_TRUE(cleaned.find("MEDIA:") == std::string::npos);
}

TEST(BaseDepthExtractLocalFiles, SkipsCodeSpans) {
    const std::string content =
        "See /tmp/a.png for details.\n"
        "`/not-in-code.png` is inline code so skip.\n"
        "```\n/fenced/b.png\n```\n"
        "Also /home/me/c.jpg is fine.";
    const auto [paths, cleaned] = bd::extract_local_files_raw(content);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], "/tmp/a.png");
    EXPECT_EQ(paths[1], "/home/me/c.jpg");
    // Code-span content remains in cleaned text.
    EXPECT_TRUE(cleaned.find("/not-in-code.png") != std::string::npos);
    EXPECT_TRUE(cleaned.find("/fenced/b.png") != std::string::npos);
}

TEST(BaseDepthCommand, IsCommandAndName) {
    EXPECT_TRUE(bd::is_command("/reset"));
    EXPECT_FALSE(bd::is_command(" /reset"));

    EXPECT_EQ(bd::get_command("/reset").value_or(""), "reset");
    EXPECT_EQ(bd::get_command("/reset@hermes_bot arg").value_or(""), "reset");
    EXPECT_FALSE(bd::get_command("/").has_value());
    EXPECT_FALSE(bd::get_command("/not/a/command").has_value());
    EXPECT_EQ(bd::get_command_args("/cmd  hello world"), "hello world");
    EXPECT_EQ(bd::get_command_args("plain text"), "plain text");
}

TEST(BaseDepthRetry, ClassifiesPatterns) {
    EXPECT_TRUE(bd::is_retryable_error("ConnectError: host unreachable"));
    EXPECT_TRUE(bd::is_retryable_error("network failure"));
    EXPECT_TRUE(bd::is_retryable_error("ConnectTimeout while dialing"));
    EXPECT_FALSE(bd::is_retryable_error("403 Forbidden"));
    EXPECT_FALSE(bd::is_retryable_error(""));
    // Timeout classification.
    EXPECT_TRUE(bd::is_timeout_error("request timed out"));
    EXPECT_TRUE(bd::is_timeout_error("ReadTimeout"));
    EXPECT_FALSE(bd::is_timeout_error("ConnectError"));
    // Plain "timed out" is *not* classified as retryable (by design).
    EXPECT_FALSE(bd::is_retryable_error("request timed out"));
}

TEST(BaseDepthMergeCaption, DoesNotDuplicateExactLine) {
    EXPECT_EQ(bd::merge_caption({}, "Hello"), "Hello");
    EXPECT_EQ(bd::merge_caption("First", "First"), "First");
    EXPECT_EQ(bd::merge_caption("First", "Second"), "First\n\nSecond");
    // Shorter caption embedded as substring is NOT treated as duplicate.
    EXPECT_EQ(bd::merge_caption("Meeting agenda", "Meeting"),
              "Meeting agenda\n\nMeeting");
}

TEST(BaseDepthTruncateMessage, ShortContentIsSingleChunk) {
    const auto chunks = bd::truncate_message("tiny", 4096);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "tiny");
}

TEST(BaseDepthTruncateMessage, SplitsAndAddsIndicators) {
    const std::string body(5000, 'a');
    const auto chunks = bd::truncate_message(body, 1000);
    EXPECT_GE(chunks.size(), 2u);
    for (std::size_t i{0}; i < chunks.size(); ++i) {
        EXPECT_LE(chunks[i].size(), 1010u);  // 10 reserved for indicator
        // Each chunk must carry an indicator.
        EXPECT_NE(chunks[i].find("/" + std::to_string(chunks.size()) + ")"),
                   std::string::npos);
    }
}

TEST(BaseDepthTruncateMessage, PreservesCodeFence) {
    std::string body = "```python\n";
    body.append(5000, 'x');
    body += "\n```";
    const auto chunks = bd::truncate_message(body, 2000);
    EXPECT_GE(chunks.size(), 2u);
    // First chunk opens with ``` but must have a closing fence appended.
    EXPECT_EQ(chunks[0].compare(0, 3, "```"), 0);
    EXPECT_NE(chunks[0].find("\n```"), std::string::npos);
    // Subsequent chunk reopens the language tag.
    EXPECT_NE(chunks[1].find("```python"), std::string::npos);
}

TEST(BaseDepthClassifyAddress, Loopback) {
    EXPECT_EQ(bd::classify_address("127.0.0.1"), bd::AddressKind::Ipv4Loopback);
    EXPECT_EQ(bd::classify_address("127.1.2.3"), bd::AddressKind::Ipv4Loopback);
    EXPECT_EQ(bd::classify_address("0.0.0.0"), bd::AddressKind::Ipv4Unspecified);
    EXPECT_EQ(bd::classify_address("8.8.8.8"), bd::AddressKind::Ipv4Public);
    EXPECT_EQ(bd::classify_address("::1"), bd::AddressKind::Ipv6Loopback);
    EXPECT_EQ(bd::classify_address("::ffff:127.0.0.1"),
              bd::AddressKind::Ipv6MappedLoopback);
    EXPECT_EQ(bd::classify_address("not-an-ip"), bd::AddressKind::Invalid);
}

TEST(BaseDepthIsNetworkAccessible, LoopbackVariants) {
    EXPECT_FALSE(bd::is_network_accessible("127.0.0.1"));
    EXPECT_FALSE(bd::is_network_accessible("::1"));
    EXPECT_FALSE(bd::is_network_accessible("::ffff:127.0.0.1"));
    EXPECT_FALSE(bd::is_network_accessible("localhost"));
    EXPECT_TRUE(bd::is_network_accessible("8.8.8.8"));
    EXPECT_TRUE(bd::is_network_accessible("0.0.0.0"));
    EXPECT_FALSE(bd::is_network_accessible(""));
}

TEST(BaseDepthParseUrl, NoSchemeFallsThrough) {
    const auto p = bd::parse_url("bare text");
    EXPECT_FALSE(p.has_scheme_netloc);
}
