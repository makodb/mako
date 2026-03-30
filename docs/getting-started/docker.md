# Docker Build Environment for Mako

This directory contains Docker configurations for building and testing Mako on Ubuntu 22.04.

## Quick Start

### One-liner Build (Non-reusable)
```bash
docker run --rm -v $(pwd):/workspace ubuntu:22.04 bash -c "apt-get update && apt-get install -y sudo curl && bash apt_packages.sh && curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y && source \$HOME/.cargo/env && cd /workspace && mkdir -p build && cd build && cmake .. && make -j32 dbtest"
```

## Reusable Docker Setup

### 1. Build the Docker Image
```bash
# Basic image
docker build -f Dockerfile.ubuntu22 -t mako-build:ubuntu22 .

# Or optimized image with better caching
docker build -f Dockerfile.ubuntu22.optimized -t mako-build:ubuntu22-opt .
```

### 2. Use the Convenience Script
```bash
# Make the script executable
chmod +x docker_build.sh

# Build the Docker image
./docker_build.sh build-image

# Build dbtest in container
./docker_build.sh build

# Run quick test (libmako.a only)
./docker_build.sh test

# Start interactive shell
./docker_build.sh shell

# Clean build artifacts
./docker_build.sh clean
```

### 3. Manual Docker Commands

#### Build dbtest
```bash
docker run --rm -v $(pwd):/workspace mako-build:ubuntu22 \
    bash -c "cd /workspace && rm -rf build && mkdir -p build && cd build && \
             cmake .. && \
             make -j32 dbtest"
```

#### Interactive Development
```bash
# Start interactive container
docker run --rm -it -v $(pwd):/workspace janus-build:ubuntu22 /bin/bash

# Inside container, you can use these aliases:
build-dbtest  # Full rebuild of dbtest
build-quick   # Quick rebuild in existing build directory
```

### 4. Docker Compose (Persistent Container)

#### Start development container
```bash
docker-compose up -d ubuntu22-dev
```

#### Connect to container
```bash
docker exec -it mako-ubuntu22-dev /bin/bash
```

#### Stop container
```bash
docker-compose down
```

## Files

- `Dockerfile.ubuntu22` - Basic Ubuntu 22.04 build environment
- `Dockerfile.ubuntu22.optimized` - Optimized multi-stage build with better caching
- `docker-compose.yml` - Docker Compose configuration for persistent containers
- `docker_build.sh` - Convenience script for common operations
- `.github/workflows/ubuntu-build.yml` - GitHub Actions CI workflow

## Troubleshooting

### Build Failures
If the build fails with rustycpp errors:
1. The borrow checking is disabled by default in the Docker environment
2. Check that the rustycpp submodule is updated: `git submodule update --init --recursive`

### Out of Memory
If the build fails due to memory issues:
- Reduce parallel jobs: `./docker_build.sh build 8` (uses 8 jobs instead of 32)
- Increase Docker memory allocation in Docker Desktop settings

### Permission Issues
If you encounter permission issues with build artifacts:
```bash
# Fix ownership after build
sudo chown -R $(id -u):$(id -g) build/
```

## Build Verification

The build has been verified to work with:
- Ubuntu 22.04 (ubuntu:22.04 Docker image)
- GCC 11.x that comes with Ubuntu 22.04
- CMake 3.22+
- Rust 1.70+

## Known Issues

1. **rustycpp borrow checking**: Currently disabled due to compatibility issues with compiler intrinsics
2. **Build time**: Full build can take 10-30 minutes depending on system resources
3. **Memory usage**: Building with -j32 requires at least 16GB RAM