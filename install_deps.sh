#!/usr/bin/env bash
set -euo pipefail

echo "Installing dependencies for TVStreamer5..."

sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-ugly \
    libgstrtspserver-1.0-dev \
    libcurl4-openssl-dev \
    libjsoncpp-dev \
    libboost-system-dev \
    libboost-thread-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libboost-dev \
    libssl-dev \
    git \
    wget \
    ca-certificates

sudo apt-get clean

echo "Dependencies installed. Build with: mkdir -p build && cd build && cmake .. && make -j4"