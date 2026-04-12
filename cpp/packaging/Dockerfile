FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y cmake g++ libsqlite3-dev libssl-dev libyaml-cpp-dev nlohmann-json3-dev
COPY cpp/ /build/cpp/
WORKDIR /build/cpp
RUN cmake --preset release && cmake --build build/release -j$(nproc)

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libsqlite3-0 libssl3 libyaml-cpp0.8 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/cpp/build/release/cli/hermes_cpp /usr/local/bin/hermes
ENTRYPOINT ["hermes"]
