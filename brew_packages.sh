#!/bin/bash
# MacOS Homebrew package installation script for Mako

echo "Installing Mako dependencies for macOS..."

# Check if Homebrew is installed
if ! command -v brew &> /dev/null; then
    echo "Homebrew not found. Please install it from https://brew.sh/"
    exit 1
fi

echo "Updating Homebrew..."
brew update

echo "Installing Build Tools..."
brew install cmake ninja pkg-config autoconf automake libtool

echo "Installing Core Libraries..."
brew install boost
brew install boost-python3
brew install yaml-cpp
brew install jemalloc
brew install gperftools
brew install gflags
brew install libevent
brew install openssl
brew install protobuf
brew install rocksdb
brew install lz4

echo "Installing Testing Frameworks..."
brew install googletest

echo "Installing Languages..."
brew install python@3.11
# Rust is usually installed via rustup, but brew works too
brew install rust

echo "Installing Utilities..."
brew install the_silver_searcher
brew install coreutils
brew install gnu-sed
brew install wget

# Note on Missing Libraries:
# - libaio: Linux-specific (Asynchronous I/O). On macOS, POSIX AIO or GCD is used.
# - libnuma: Linux-specific (NUMA topology). macOS has no direct equivalent; stubbed out.
# - libibverbs/libmnl/libdpdk: RDMA/DPDK are Linux-specific.
# - libsystemd: Linux-specific.

echo "
To build Mako on macOS, you may need to export:
  export CPATH=/opt/homebrew/include
  export LIBRARY_PATH=/opt/homebrew/lib
  export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig
"

echo "Installation complete."
