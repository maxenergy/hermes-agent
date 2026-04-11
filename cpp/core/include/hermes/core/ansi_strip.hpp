// Strip ECMA-48 / VT100 escape sequences from arbitrary text.
#pragma once

#include <string>
#include <string_view>

namespace hermes::core::ansi_strip {

// Remove CSI (`ESC [ ... letter`), OSC (`ESC ] ... BEL` or `ESC ]...ESC\`),
// two-byte escapes (`ESC letter`), and 8-bit C1 bytes (0x80-0x9F).
// Implementation is a tiny state machine — robust against binary input.
std::string strip_ansi(std::string_view input);

}  // namespace hermes::core::ansi_strip
