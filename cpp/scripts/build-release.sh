#!/bin/bash
# Build release artifacts for the current platform.
set -euo pipefail
cd "$(dirname "$0")/.."

rm -rf build/release
cmake --preset release
cmake --build build/release -j
strip build/release/cli/hermes_cpp || true

echo "Binary: build/release/cli/hermes_cpp"
file build/release/cli/hermes_cpp
ls -lh build/release/cli/hermes_cpp
