#include "hermes/tools/binary_extensions.hpp"

#include <gtest/gtest.h>

using hermes::tools::is_binary_extension;
using hermes::tools::is_likely_binary_content;

TEST(BinaryExtensions, KnownBinaryExtensions) {
    EXPECT_TRUE(is_binary_extension("photo.jpg"));
    EXPECT_TRUE(is_binary_extension("document.pdf"));
    EXPECT_TRUE(is_binary_extension("archive.zip"));
}

TEST(BinaryExtensions, TextExtensionsAreFalse) {
    EXPECT_FALSE(is_binary_extension("readme.txt"));
    EXPECT_FALSE(is_binary_extension("notes.md"));
    EXPECT_FALSE(is_binary_extension("script.py"));
}

TEST(BinaryExtensions, NullByteDetectedAsBinary) {
    std::string content = "hello";
    content.push_back('\0');
    content.append("world");
    EXPECT_TRUE(is_likely_binary_content(content));
}

TEST(BinaryExtensions, PlainTextNotBinary) {
    EXPECT_FALSE(is_likely_binary_content("Hello, world!\n"));
}
