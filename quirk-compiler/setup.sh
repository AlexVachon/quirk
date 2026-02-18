#!/usr/bin/env bash

set -e  # Stop on first error

echo "Updating system packages..."
sudo apt-get update
sudo apt-get install -y cmake pkg-config zip unzip tar git

# Clone vcpkg if it doesn't exist
if [ ! -d "vcpkg" ]; then
    echo "Cloning vcpkg..."
    git clone https://github.com/microsoft/vcpkg.git
else
    echo "vcpkg already exists, skipping clone."
fi

# Bootstrap vcpkg
echo "Bootstrapping vcpkg..."
./vcpkg/bootstrap-vcpkg.sh

# Configure project with CMake
echo "Configuring project..."
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake

# Build project
echo "Building project..."
cmake --build build -j$(nproc)

echo "Setup complete!"
