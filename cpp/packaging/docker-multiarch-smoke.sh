#!/usr/bin/env bash
# cpp/packaging/docker-multiarch-smoke.sh
#
# Smoke-test the multi-arch Docker image produced by
# `.github/workflows/cpp-ci.yml :: docker-multiarch` (introduced in
# 78728392) and/or by `cpp/packaging/docker-buildx.sh` locally.
#
# Strategy:
#   1. Build (or pull, with --image) each platform separately with
#      --load so the image lands in the local docker daemon. buildx's
#      --load flag only accepts a single platform per invocation, so we
#      loop over amd64 and arm64 one at a time.
#   2. Run `hermes_cpp --version` inside each arch image via
#      `docker run --platform=<plat>`. This exercises QEMU emulation
#      for the non-native arch and confirms the binary actually boots.
#   3. Exit non-zero if any arch fails to build or produces no output.
#
# Usage:
#   ./cpp/packaging/docker-multiarch-smoke.sh                 # build locally
#   ./cpp/packaging/docker-multiarch-smoke.sh --image ghcr.io/owner/hermes:tag
#   PLATFORMS="linux/amd64 linux/arm64 linux/arm/v7" ./... # override arch list

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DOCKERFILE="$SCRIPT_DIR/Dockerfile"

IMAGE=""
# shellcheck disable=SC2206 # intentional word-split
PLATFORMS=(${PLATFORMS:-linux/amd64 linux/arm64})
BUILDER_NAME="hermes-multiarch-smoke"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --platforms) read -r -a PLATFORMS <<<"$2"; shift 2 ;;
        -h|--help)
            sed -n '2,25p' "$0"
            exit 0
            ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo "[smoke] ERROR: docker not installed" >&2
    exit 2
fi

if ! docker buildx version >/dev/null 2>&1; then
    echo "[smoke] ERROR: docker buildx plugin not installed" >&2
    exit 2
fi

# Register QEMU binfmt handlers for emulated arches. Idempotent.
if [[ ${#PLATFORMS[@]} -gt 1 ]]; then
    echo "[smoke] Registering QEMU binfmt handlers (tonistiigi/binfmt)"
    docker run --privileged --rm tonistiigi/binfmt --install all >/dev/null
fi

# Ensure a container-driver builder exists so multi-arch builds work.
if [[ -z "$IMAGE" ]]; then
    if ! docker buildx inspect "$BUILDER_NAME" >/dev/null 2>&1; then
        echo "[smoke] Creating builder '$BUILDER_NAME'"
        docker buildx create --name "$BUILDER_NAME" --driver docker-container --use >/dev/null
    else
        docker buildx use "$BUILDER_NAME" >/dev/null
    fi
    docker buildx inspect --bootstrap >/dev/null
fi

failures=0
for platform in "${PLATFORMS[@]}"; do
    arch_tag="hermes-cpp:smoke-${platform//\//-}"
    if [[ -n "$IMAGE" ]]; then
        echo "[smoke] === $platform === pulling $IMAGE"
        docker pull --platform="$platform" "$IMAGE"
        run_image="$IMAGE"
    else
        echo "[smoke] === $platform === buildx --load $arch_tag"
        docker buildx build \
            --platform "$platform" \
            --load \
            -f "$DOCKERFILE" \
            -t "$arch_tag" \
            "$REPO_ROOT"
        run_image="$arch_tag"
    fi

    echo "[smoke] Running hermes_cpp --version under $platform"
    # The --version probe should exit 0 and emit at least one line. If
    # the binary is not the entrypoint, try `hermes --version` as a
    # fallback so the script works for either Dockerfile layout.
    out=""
    if out=$(docker run --rm --platform="$platform" "$run_image" --version 2>&1); then
        :
    elif out=$(docker run --rm --platform="$platform" "$run_image" hermes --version 2>&1); then
        :
    else
        echo "[smoke] FAIL: $platform did not respond to --version"
        echo "------ output ------"
        echo "$out"
        echo "--------------------"
        failures=$((failures + 1))
        continue
    fi

    if [[ -z "$out" ]]; then
        echo "[smoke] FAIL: $platform --version printed nothing"
        failures=$((failures + 1))
        continue
    fi

    echo "[smoke] OK $platform -> $out"
done

if [[ $failures -gt 0 ]]; then
    echo "[smoke] $failures architecture(s) failed" >&2
    exit 1
fi

echo "[smoke] All ${#PLATFORMS[@]} architecture(s) passed."
