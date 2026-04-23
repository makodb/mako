#!/bin/bash
# Package installation script for Ubuntu 24.04 (Noble Numbat)
#
# For Ubuntu 22.04, uncomment the following GCC PPA and LLVM repo lines:
#   sudo apt-get --assume-yes install software-properties-common
#   sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
#   sudo apt-get update
#   sudo apt-get --assume-yes install gcc-12 g++-12
#   sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100
#   sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100
#   # LLVM 16 for Ubuntu 22.04:
#   wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
#   echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-16 main" | sudo tee /etc/apt/sources.list.d/llvm-16.list
#   sudo apt-get update
#   sudo apt-get --assume-yes install llvm-16-dev libclang-16-dev libz3-dev

export DEBIAN_FRONTEND=noninteractive
sudo apt-get update

# Build tools
sudo apt-get --assume-yes install make automake cmake ninja-build pkg-config autoconf

# Memory allocators and profiling
sudo apt-get --assume-yes install libjemalloc-dev libgoogle-perftools-dev

# Testing frameworks
sudo apt-get --assume-yes install googletest libgtest-dev libgmock-dev

# Development libraries
sudo apt-get --assume-yes install libaio-dev libssl-dev libffi-dev python3-dev
sudo apt-get --assume-yes install libprotobuf-dev librocksdb-dev
sudo apt-get --assume-yes install libnuma-dev libibverbs-dev libgflags-dev libevent-dev
sudo apt-get --assume-yes install libpmem-dev libsystemd-dev

# Networking
sudo apt-get --assume-yes install libnl-3-dev libnl-route-3-dev net-tools

# Tools
sudo apt-get --assume-yes install silversearcher-ag numactl cgroup-tools strace sshpass valgrind
sudo apt-get --assume-yes install gh openssh-server

# Python
sudo apt-get --assume-yes install python3 python3-pip python3-dev cython3 python3-docutils
sudo ln -sf /usr/bin/python3 /usr/bin/python

# Documentation
sudo apt-get --assume-yes install pandoc

# DPDK (if available)
sudo apt-get --assume-yes install libdpdk-dev || echo "DPDK not available, skipping"

# For rusty-cpp-checker: LLVM 18 and Z3 (available in Ubuntu 24.04 default repos)
sudo apt-get --assume-yes install llvm-18-dev libclang-18-dev clang-18 libz3-dev
# Required by CMakeLists.txt forcing clang + -stdlib=libc++
sudo apt-get --assume-yes install libc++-18-dev libc++abi-18-dev

# Set up alternatives for clang/llvm
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 || true
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 || true
sudo update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-18 100 || true

echo "Package installation complete"
