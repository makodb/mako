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
sudo apt-get --assume-yes install make automake cmake ninja-build pkg-config autoconf curl

# Ensure CMake >= 3.30 for C++23 import std/module support.
# Ubuntu 24.04 apt currently provides 3.28.x, which is too old for this repo.
REQUIRED_CMAKE_VERSION="3.30.0"
BOOTSTRAP_CMAKE_VERSION="${BOOTSTRAP_CMAKE_VERSION:-3.31.0}"

version_ge() {
    # Returns success when $1 >= $2.
    [ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}

current_cmake_version="0.0.0"
if command -v cmake >/dev/null 2>&1; then
    current_cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
fi

if ! version_ge "$current_cmake_version" "$REQUIRED_CMAKE_VERSION"; then
    echo "Installing newer CMake (have ${current_cmake_version}, need >= ${REQUIRED_CMAKE_VERSION})..."

    case "$(uname -m)" in
        x86_64|amd64) cmake_arch="x86_64" ;;
        aarch64|arm64) cmake_arch="aarch64" ;;
        *)
            echo "Unsupported architecture for CMake bootstrap: $(uname -m)"
            exit 1
            ;;
    esac

    cmake_tar="cmake-${BOOTSTRAP_CMAKE_VERSION}-linux-${cmake_arch}.tar.gz"
    cmake_url="https://github.com/Kitware/CMake/releases/download/v${BOOTSTRAP_CMAKE_VERSION}/${cmake_tar}"
    tmp_dir="$(mktemp -d)"

    curl -fsSL "$cmake_url" -o "${tmp_dir}/${cmake_tar}"
    sudo rm -rf "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}"
    sudo mkdir -p "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}"
    sudo tar -xzf "${tmp_dir}/${cmake_tar}" --strip-components=1 -C "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}"
    sudo ln -sf "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}/bin/cmake" /usr/local/bin/cmake
    sudo ln -sf "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}/bin/ctest" /usr/local/bin/ctest
    sudo ln -sf "/opt/cmake-${BOOTSTRAP_CMAKE_VERSION}/bin/cpack" /usr/local/bin/cpack
    rm -rf "$tmp_dir"
fi

echo "Using $(cmake --version | head -n1)"

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

# Clang 21 from apt.llvm.org (Ubuntu 24.04 noble ships clang 18, which doesn't
# support alias-template CTAD that the rusty-cpp platform::threading aliases
# rely on — see CMakeLists.txt compiler-version check).
sudo apt-get --assume-yes install software-properties-common gnupg
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
    | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" \
    | sudo tee /etc/apt/sources.list.d/llvm-21.list > /dev/null
sudo apt-get update

# For rusty-cpp-checker and CMake module scanning: LLVM/Clang tools + Z3.
sudo apt-get --assume-yes install llvm-21-dev libclang-21-dev clang-21 clang-tools-21 libz3-dev
# Required by CMakeLists.txt forcing clang + -stdlib=libc++
sudo apt-get --assume-yes install libc++-21-dev libc++abi-21-dev

# Set up alternatives for clang/llvm — clang-21 wins, with clang-18 left as a
# lower-priority fallback if it happens to be installed.
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 200 || true
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 200 || true
sudo update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-21 200 || true

echo "Package installation complete"
