#!/data/data/com.termux/files/usr/bin/env bash
# cpp/packaging/termux/build.sh
#
# Build the hermes C++ port under Termux on Android (aarch64 / armv7).
# Produces $BUILD_DIR/cli/hermes_cpp — a Termux-native binary linked
# against Bionic (not glibc).
#
# Usage:
#   pkg update && pkg upgrade -y
#   pkg install -y cmake clang ninja git pkg-config \
#                  openssl libcurl libssh2 \
#                  sqlite libyaml yaml-cpp \
#                  boost libuv zlib
#   # Optional (unlock more features):
#   pkg install -y libopus libolm
#   bash cpp/packaging/termux/build.sh
#
# Termux peculiarities the build tolerates:
#   * No /bin/bash, no /usr/bin/env — shebang hard-codes $PREFIX path.
#   * No systemd, no launchd — the installer only drops a binary in
#     $PREFIX/bin. See termux-install.sh.
#   * Bionic libc: no glibc-specific syscalls. Code paths that need
#     them are gated behind `#ifdef __ANDROID__` at compile time.
#   * PTY support is limited — ForegroundTimeout/CopyPaste tests are
#     excluded from ctest along with the other heavy integration tests.
#   * /proc has Android-specific restrictions but /proc/self/status and
#     /proc/self/cmdline work fine for gateway status introspection.
#   * Host triple: aarch64-linux-android (or armv7a-linux-androideabi
#     for the 32-bit ARM builds Termux still supports on legacy devices).

set -euo pipefail

: "${PREFIX:?Termux PREFIX not set — are you running inside Termux?}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$CPP_DIR"

BUILD_DIR="${BUILD_DIR:-build/termux}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "[termux-build] CPP_DIR   = $CPP_DIR"
echo "[termux-build] BUILD_DIR = $BUILD_DIR"
echo "[termux-build] PREFIX    = $PREFIX"
echo "[termux-build] JOBS      = $JOBS"

# Preflight: refuse to run under Debian proot or Ubuntu chroot — those
# appear to be Termux but use glibc, which produces binaries that won't
# run from a fresh Termux session.
if [[ -f "$PREFIX/etc/debian_version" ]]; then
    echo "[termux-build] ERROR: detected Debian/Ubuntu rootfs at \$PREFIX."
    echo "                Run this script from native Termux, not proot-distro."
    exit 2
fi

echo "[termux-build] Configuring"
cmake -S . -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER="$PREFIX/bin/clang" \
    -DCMAKE_CXX_COMPILER="$PREFIX/bin/clang++" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DHERMES_TERMUX=ON \
    -DHERMES_ANDROID=ON

echo "[termux-build] Building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j"$JOBS"

if [[ "${HERMES_RUN_TESTS:-0}" == "1" ]]; then
    echo "[termux-build] Running tests (Termux-safe subset)"
    ctest --test-dir "$BUILD_DIR" \
        --exclude-regex "CancelFn|ForegroundTimeout|CopyPaste|SkillsHub|Live|IMAP_TEST|SINGULARITY_TEST|PTY|WSL2_TEST" \
        --output-on-failure || true
fi

BIN="$BUILD_DIR/cli/hermes_cpp"
if [[ -x "$BIN" ]]; then
    echo "[termux-build] Built: $BIN"
    file "$BIN" 2>/dev/null || true
    echo "[termux-build] Install with:"
    echo "    bash cpp/packaging/termux/termux-install.sh"
else
    echo "[termux-build] ERROR: expected binary at $BIN — build failed?"
    exit 3
fi
