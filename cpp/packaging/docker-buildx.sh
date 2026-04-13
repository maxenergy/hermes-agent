#!/usr/bin/env bash
# Build multi-arch (linux/amd64 + linux/arm64) hermes C++ images via buildx.
#
# Usage:
#   ./cpp/packaging/docker-buildx.sh [--push] [--tag hermes:latest]
#
# Requires `docker buildx` (included with Docker Desktop, or `docker-buildx-plugin`
# apt package on Linux). QEMU binfmt handlers must be registered for cross-arch
# emulation — run once on the host:
#   docker run --privileged --rm tonistiigi/binfmt --install all

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TAG="hermes:latest"
PUSH=0
PLATFORMS="linux/amd64,linux/arm64"
BUILDER_NAME="hermes-multiarch"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --push) PUSH=1; shift ;;
        --tag)  TAG="$2"; shift 2 ;;
        --platforms) PLATFORMS="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

if ! docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
    echo "[buildx] Creating builder '$BUILDER_NAME'"
    docker buildx create --name "$BUILDER_NAME" --driver docker-container --use
else
    docker buildx use "$BUILDER_NAME"
fi

docker buildx inspect --bootstrap >/dev/null

OUTPUT_FLAG="--load"
if [[ $PUSH -eq 1 ]]; then
    OUTPUT_FLAG="--push"
fi

# Note: --load only works for a single platform. When building multi-arch
# without pushing, we emit to the OCI archive instead.
if [[ $PUSH -eq 0 && "$PLATFORMS" == *","* ]]; then
    OUTPUT_FLAG="--output=type=oci,dest=hermes-multiarch.oci.tar"
    echo "[buildx] Multi-arch local build — writing OCI archive to hermes-multiarch.oci.tar"
fi

cd "$REPO_ROOT"
docker buildx build \
    --platform "$PLATFORMS" \
    -f cpp/packaging/Dockerfile \
    -t "$TAG" \
    $OUTPUT_FLAG \
    .
