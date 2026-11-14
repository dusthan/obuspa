# Linux build/test environment for the CPE simulator (source tree is mounted at /src).
#   docker build -t obuspa-sim-dev test/sim/docker
#   test/sim/docker/run.sh '<command>'
FROM debian:stable

RUN apt-get update && apt-get -y install --no-install-recommends \
    build-essential autoconf automake libtool pkg-config cmake git ca-certificates \
    libssl-dev libcurl4-openssl-dev libsqlite3-dev zlib1g-dev libmosquitto-dev \
    mosquitto sqlite3 procps psmisc python3 \
 && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 -b v4.3.3 https://github.com/warmcat/libwebsockets.git /tmp/lws \
 && cmake -S /tmp/lws -B /tmp/lws/build -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
 && cmake --build /tmp/lws/build -j"$(nproc)" && cmake --install /tmp/lws/build \
 && ldconfig && rm -rf /tmp/lws

WORKDIR /src
