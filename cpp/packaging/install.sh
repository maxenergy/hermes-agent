#!/bin/bash
set -euo pipefail

echo "Hermes C++ installer"

ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')

echo "Detected: OS=${OS} ARCH=${ARCH}"

INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
HERMES_HOME="${HERMES_HOME:-$HOME/.hermes}"

# TODO: download pre-built binary from release artifacts.
# BASE_URL="https://github.com/NousResearch/hermes-agent/releases/latest/download"
# BINARY="hermes-${OS}-${ARCH}"
# curl -fsSL "${BASE_URL}/${BINARY}" -o "${INSTALL_DIR}/hermes"
# chmod +x "${INSTALL_DIR}/hermes"

mkdir -p "${HERMES_HOME}/plugins"

echo "Installation complete. Run 'hermes' to start."
