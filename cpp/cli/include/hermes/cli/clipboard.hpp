// Clipboard helpers — copy/paste via platform-native CLI tools.
#pragma once

#include <string>

namespace hermes::cli {

// Copy text to the system clipboard.
// Linux: xclip -selection clipboard (falls back to xsel)
// macOS: pbcopy
// Returns true on success.
bool copy_to_clipboard(const std::string& text);

// Paste text from the system clipboard.
// Linux: xclip -selection clipboard -o (falls back to xsel -ob)
// macOS: pbpaste
// Returns empty string on failure.
std::string paste_from_clipboard();

}  // namespace hermes::cli
