#include "hermes/core/ansi_strip.hpp"

#include <cstdint>
#include <string>

namespace hermes::core::ansi_strip {

namespace {

constexpr char kEsc = 0x1B;  // ESC
constexpr char kBel = 0x07;  // BEL

enum class State {
    Normal,
    AfterEsc,     // saw ESC
    CsiParams,    // saw ESC [
    Osc,          // saw ESC ]
    OscEsc,       // inside OSC, saw ESC (maybe ST)
};

}  // namespace

std::string strip_ansi(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    State state = State::Normal;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(input[i]);
        switch (state) {
            case State::Normal: {
                if (byte == static_cast<std::uint8_t>(kEsc)) {
                    state = State::AfterEsc;
                } else if (byte >= 0x80 && byte <= 0x9F) {
                    // 8-bit C1 controls — drop. If it's CSI (0x9B),
                    // consume the rest of the sequence too.
                    if (byte == 0x9B) {
                        state = State::CsiParams;
                    } else if (byte == 0x9D) {
                        state = State::Osc;
                    }
                    // Others (e.g. 0x84..0x9A) are simple — just skip.
                } else {
                    out.push_back(static_cast<char>(byte));
                }
                break;
            }
            case State::AfterEsc: {
                if (byte == '[') {
                    state = State::CsiParams;
                } else if (byte == ']') {
                    state = State::Osc;
                } else if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
                    // DCS/SOS/PM/APC — treated like OSC for termination.
                    state = State::Osc;
                } else {
                    // Simple two-byte escape (ESC letter) — drop and return.
                    state = State::Normal;
                }
                break;
            }
            case State::CsiParams: {
                // CSI: parameter bytes 0x30-0x3F, intermediate 0x20-0x2F,
                // then a single final byte in 0x40-0x7E.
                if (byte >= 0x40 && byte <= 0x7E) {
                    state = State::Normal;
                }
                break;
            }
            case State::Osc: {
                if (byte == static_cast<std::uint8_t>(kBel)) {
                    state = State::Normal;
                } else if (byte == static_cast<std::uint8_t>(kEsc)) {
                    state = State::OscEsc;
                }
                break;
            }
            case State::OscEsc: {
                // ESC \ is ST (string terminator); any other byte reverts
                // to OSC parsing to keep us conservative on malformed input.
                if (byte == '\\') {
                    state = State::Normal;
                } else {
                    state = State::Osc;
                }
                break;
            }
        }
    }
    return out;
}

}  // namespace hermes::core::ansi_strip
