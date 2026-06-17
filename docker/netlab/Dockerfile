FROM debian:bookworm

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    git \
    iproute2 \
    iptables \
    ninja-build \
    pkg-config \
    python3 \
    libssl-dev \
    libsodium-dev \
    libzstd-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt /src/
COPY src /src/src
COPY tests /src/tests
COPY scripts/tui_smoke.py /src/scripts/tui_smoke.py

RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  && cmake --build /build --target kiko

COPY docker/netlab /src/docker/netlab

RUN cp /build/kiko /usr/local/bin/kiko \
  && cp /src/docker/netlab/netlab-*.sh /usr/local/bin/ \
  && chmod +x /usr/local/bin/netlab-*.sh

WORKDIR /work
