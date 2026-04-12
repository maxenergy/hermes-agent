# Multi-arch Dockerfile for the hermes C++ binary.
# Build with:  ./docker-buildx.sh
# or:           docker buildx build --platform linux/amd64,linux/arm64 -f cpp/packaging/Dockerfile -t hermes:latest .

ARG TARGETARCH
FROM --platform=$BUILDPLATFORM ubuntu:24.04 AS builder
ARG TARGETARCH
ARG TARGETPLATFORM
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake g++ git pkg-config \
        libsqlite3-dev libssl-dev libyaml-cpp-dev \
        nlohmann-json3-dev libcurl4-openssl-dev && \
    rm -rf /var/lib/apt/lists/*

COPY cpp/ /build/cpp/
WORKDIR /build/cpp

RUN echo "Building for TARGETARCH=$TARGETARCH TARGETPLATFORM=$TARGETPLATFORM" && \
    cmake --preset release && \
    cmake --build build/release -j"$(nproc)"

# Runtime stage — minimal footprint.
FROM ubuntu:24.04 AS runtime
ARG TARGETARCH
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libssl3 libyaml-cpp0.8 libcurl4 ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/cpp/build/release/cli/hermes_cpp /usr/local/bin/hermes
LABEL org.opencontainers.image.source="https://github.com/NousResearch/hermes-agent"
LABEL org.opencontainers.image.title="hermes-cpp"
LABEL org.opencontainers.image.description="Hermes agent (C++ build) — arch=${TARGETARCH}"
ENTRYPOINT ["hermes"]
