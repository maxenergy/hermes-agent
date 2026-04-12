#!/data/data/com.termux/files/usr/bin/env bash
# Build the hermes C++ binary under Termux on Android (aarch64/arm).
#
# Prerequisites (run once):
#   pkg update && pkg upgrade
#   pkg install cmake clang git make pkg-config \
#               openssl sqlite libcurl nlohmann-json \
#               libyaml-cpp
#
# Termux peculiarities handled here:
#   * No /bin/bash — Termux ships bash under $PREFIX.
#   * No systemd — we only build the binary; no service install.
#   * PTY support is limited; PTY-based tests are skipped.
#   * /proc has restrictions under Android but enough for our gateway status.
#   * Host triple is aarch64-linux-android (or armv7a- for 32-bit).

set -euo pipefail

: "${PREFIX:?Termux PREFIX not set — are you running inside Termux?}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$CPP_DIR"

BUILD_DIR="${BUILD_DIR:-build/termux}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

echo "[termux-build] Configuring in $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$PREFIX/bin/clang" \
    -DCMAKE_CXX_COMPILER="$PREFIX/bin/clang++" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DHERMES_TERMUX=ON

echo "[termux-build] Building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j"$JOBS"

if [[ "${HERMES_RUN_TESTS:-0}" == "1" ]]; then
    echo "[termux-build] Running tests (excluding PTY/foreground-timer tests)"
    ctest --test-dir "$BUILD_DIR" \
        --exclude-regex "CancelFn|ForegroundTimeout|CopyPaste|PTY|Live" \
        --output-on-failure || true
fi

echo "[termux-build] Done. Binary: $BUILD_DIR/cli/hermes_cpp"
echo "[termux-build] Install: cp $BUILD_DIR/cli/hermes_cpp \$PREFIX/bin/hermes"
