FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    pkg-config \
    libboost-system-dev \
    libboost-thread-dev \
    libcurl4-openssl-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libjsoncpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libboost-system1.83.0 \
    libboost-thread1.83.0 \
    libcurl4 \
    libjsoncpp25 \
    libgstreamer1.0-0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/TVStreamer /app/TVStreamer

WORKDIR /data
EXPOSE 9000/tcp

ENTRYPOINT ["/app/TVStreamer"]
