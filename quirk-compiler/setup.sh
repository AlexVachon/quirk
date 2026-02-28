#!/usr/bin/env bash

set -e  # Stop on first error

echo "Updating system packages..."
sudo apt-get update
sudo apt-get install -y \
    cmake \
    pkg-config \
    zip unzip tar git \
    libcurl4-openssl-dev \
    libgc-dev \
    llvm-14 \
    libllvm14 \
    llvm-14-dev

# Wipe old build dir to avoid stale CMake cache
echo "Cleaning old build..."
rm -rf build

# Configure project with CMake (no vcpkg — all deps from apt)
echo "Configuring project..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build project
echo "Building project..."
cmake --build build -j$(nproc)

echo ""
echo "Setup complete! Binary is at ./bin/quirk"