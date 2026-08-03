#!/usr/bin/env bash
set -euo pipefail

echo "Installing dependencies for TVStreamer5..."

SUDO=()
if [[ "${EUID}" -ne 0 ]]; then
    SUDO=(sudo)
fi

APT_GET=("${SUDO[@]}" apt-get)

install_first_available() {
    local description="$1"
    shift

    for package in "$@"; do
        if apt-cache show "${package}" >/dev/null 2>&1; then
            echo "Installing ${description}: ${package}"
            "${APT_GET[@]}" install -y "${package}"
            return
        fi
    done

    echo "Unable to find package for ${description}. Tried: $*" >&2
    exit 1
}

"${APT_GET[@]}" update
"${APT_GET[@]}" install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
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

install_first_available "SRT development files" \
    libsrt-gnutls-dev \
    libsrt-openssl-dev \
    libsrt-dev

if ! pkg-config --exists srt; then
    echo "SRT pkg-config metadata was not found after installation." >&2
    echo "Install a package that provides srt.pc, then rerun CMake." >&2
    exit 1
fi

"${APT_GET[@]}" clean

echo "Dependencies installed. Build with: cmake -S . -B build && cmake --build build --parallel"
