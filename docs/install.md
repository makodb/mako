# Installation Guide

This guide will walk you through installing Mako on Debian 12 or Ubuntu 22.04 LTS.

## Prerequisites

### Supported Operating Systems

Mako has been tested and is officially supported on:
- ✅ **Debian 12** (Bookworm)
- ✅ **Ubuntu 22.04 LTS** (Jammy Jellyfish)

Other Linux distributions may work but are not officially supported.

### Hardware Requirements

**Minimum (for development/testing):**
- 4 CPU cores
- 8 GB RAM
- 20 GB disk space
- 1 Gbps network

**Recommended (for production/benchmarking):**
- 16-24 CPU cores
- 64 GB RAM
- 100 GB SSD storage
- 10 Gbps network or RDMA

### Required Software

The following will be installed during setup:
- Git
- CMake (3.15+)
- C++17 compiler (GCC 9+ or Clang 10+)
- Rust compiler (via rustup)
- Various system libraries (APR, Boost, YAML-cpp, etc.)

## Installation Steps

### Step 1: Clone the Repository

```bash
# Clone with submodules (required)
git clone --recursive https://github.com/makodb/mako.git
cd mako
```

**Important**: The `--recursive` flag is required to fetch all git submodules (Masstree, RocksDB, etc.).

If you forget the `--recursive` flag, you can initialize submodules later:
```bash
git submodule update --init --recursive
```

### Step 2: Install System Dependencies

Mako provides a script to install all required system packages:

```bash
# Install APT packages
bash apt_packages.sh
```

This script installs:
- Build tools: `git`, `pkg-config`, `build-essential`, `clang`, `cmake`
- Libraries: `libapr1-dev`, `libaprutil1-dev`, `libboost-all-dev`, `libyaml-cpp-dev`
- Python: `python3-dev`, `python3-pip`
- Performance tools: `libgoogle-perftools-dev`
- Other dependencies as needed

**Manual Installation** (if the script doesn't work):
```bash
sudo apt-get update
sudo apt-get install -y \
    git \
    pkg-config \
    build-essential \
    clang \
    cmake \
    libapr1-dev \
    libaprutil1-dev \
    libboost-all-dev \
    libyaml-cpp-dev \
    python3-dev \
    python3-pip \
    libgoogle-perftools-dev \
    libjemalloc-dev \
    libssl-dev \
    libz-dev \
    libbz2-dev \
    liblz4-dev \
    libsnappy-dev \
    libzstd-dev

# Install Python dependencies
sudo pip3 install -r requirements.txt
```

### Step 3: Install Rust Compiler

Mako uses Rust components for memory safety (RustyCpp):

```bash
# Install Rust toolchain
source install_rustc.sh
```

This script uses [rustup](https://rustup.rs/) to install the Rust compiler and Cargo build system.

**Verify Rust installation**:
```bash
rustc --version
cargo --version
```

### Step 4: Build Mako

Build Mako using the provided Makefile:

```bash
# Build with maximum parallelism (adjust -j based on your CPU cores)
make -j32

# For systems with fewer cores (e.g., laptops)
make -j4
```

**Build time**: Initial build can take 10-30 minutes depending on your system.

The build process will:
1. Configure CMake build system
2. Generate RPC stubs from `.rpc` files
3. Compile C++ source code
4. Build Rust components (RustyCpp)
5. Compile Masstree storage engine
6. Build RocksDB backend
7. Link all executables

**Build artifacts** will be in the `build/` directory:
- `build/dbtest` - Main database server executable
- `build/labtest` - Test runner executable
- Other test and benchmark executables

### Step 5: Verify Installation

Run the test suite to verify everything works:

```bash
# Run all integration tests
./ci/ci.sh all

# Or run specific tests
./ci/ci.sh simpleTransaction    # Simple transactions
./ci/ci.sh simplePaxos           # Paxos replication
./ci/ci.sh shard1Replication     # 1-shard with replication
```

If tests pass, your installation is successful! 🎉

## Build Options

### Clean Build

If you encounter build issues, try a clean build:

```bash
# Clean all build artifacts
make clean

# Rebuild from scratch
make -j32
```

The `make clean` command removes:
- CMake build directory (`build/`)
- Temporary test files in `/tmp/`
- RocksDB database files
- Masstree configuration files
- Rust build artifacts

### Build Modes

Mako supports different build configurations (via CMake flags):

```bash
# Debug build with symbols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel $(nproc)

# Release build with optimizations
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)

# RelWithDebInfo (optimized + debug symbols)
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel $(nproc)
```

### Optional Features

Enable/disable features at build time:

```bash
# Enable RustyCpp borrow checking (slower build)
cmake -S . -B build -DENABLE_BORROW_CHECKING=ON

# Build with DPDK support (kernel bypass networking)
cmake -S . -B build -DDPDK_ENABLED=ON

# Build without RocksDB persistence
cmake -S . -B build -DUSE_ROCKSDB=OFF
```

## Troubleshooting

### Common Issues

#### Issue: `git submodule` errors
**Solution**: Make sure you cloned with `--recursive` or run:
```bash
git submodule update --init --recursive
```

#### Issue: CMake version too old
```
CMake Error: CMake 3.15 or higher is required
```
**Solution**: Install newer CMake from Kitware's APT repository:
```bash
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ jammy main'
sudo apt update
sudo apt install cmake
```

#### Issue: Boost libraries not found
```
Could NOT find Boost (missing: system thread)
```
**Solution**: Install Boost development packages:
```bash
sudo apt-get install libboost-all-dev
```

#### Issue: Rust compiler not found
```
error: Rust compiler 'rustc' not found
```
**Solution**: Ensure Rust installation completed and activate in current shell:
```bash
source install_rustc.sh
source $HOME/.cargo/env
```

#### Issue: Out of memory during build
**Solution**: Reduce build parallelism:
```bash
make -j2  # Use only 2 cores instead of all cores
```

#### Issue: Permission denied errors
**Solution**: Don't run build commands with `sudo`. Build as regular user:
```bash
make -j32  # NOT: sudo make -j32
```

### Getting Help

If you encounter issues not covered here:

1. Check existing [GitHub Issues](https://github.com/makodb/mako/issues)
2. Search [GitHub Discussions](https://github.com/makodb/mako/discussions)
3. File a new issue with:
   - Your OS version (`lsb_release -a`)
   - Build log output
   - Steps to reproduce the problem

## Next Steps

Now that Mako is installed, you can:

1. **[Quick Start Tutorial](quickstart.md)** - Run your first Mako cluster
2. **[Configuration Reference](config.md)** - Learn about configuration options
3. **[Running Tests](run.md)** - Understand the test framework
4. **[EC2 Deployment](ec2.md)** - Deploy Mako on AWS

## Advanced Installation

### Installing on Older Ubuntu Versions

For Ubuntu 20.04 or older systems, see [Build Instructions](build.md) for Linuxbrew-based installation.

### Docker Installation

For containerized deployments, see:
- **[Docker Deployment](DOCKER_BUILD.md)** - Building and running in Docker
- **[Docker Verification](DOCKER_VERIFICATION.md)** - Verifying Docker builds

### Development Installation

For Mako developers, see:
- **[Development Setup](dev-setup.md)** - Setting up development environment
- **[Build System](build-system.md)** - Understanding CMake configuration

---

**Next**: [Quick Start Tutorial](quickstart.md) | [Configuration Reference](config.md)
