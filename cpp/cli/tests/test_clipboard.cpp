#include "hermes/cli/clipboard.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace hc = hermes::cli;

// Clipboard tests require a display server. Gate on DISPLAY or
// WAYLAND_DISPLAY being set.
static bool has_display() {
    return std::getenv("DISPLAY") != nullptr ||
           std::getenv("WAYLAND_DISPLAY") != nullptr;
}

TEST(Clipboard, CopyPasteRoundTrip) {
    if (!has_display()) {
        GTEST_SKIP() << "No display server — skipping clipboard test";
    }

    const std::string text = "hermes-clipboard-test-12345";
    bool ok = hc::copy_to_clipboard(text);
    if (!ok) {
        GTEST_SKIP() << "No clipboard tool (xclip/xsel/wl-copy) available";
    }

    auto result = hc::paste_from_clipboard();
    EXPECT_EQ(result, text);
}
