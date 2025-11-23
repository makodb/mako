# MAKO Database System Docker Image
# Based on Ubuntu 22.04 with amd64 architecture
FROM --platform=linux/amd64 ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Set timezone
ENV TZ=UTC
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# Update package lists
RUN apt-get update && apt-get upgrade -y

# Install basic tools and dependencies first
RUN apt-get install -y \
    software-properties-common \
    curl \
    wget \
    git \
    build-essential \
    pkg-config \
    autoconf \
    automake \
    make \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Add Ubuntu toolchain repository for GCC 12
RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test && apt-get update

# Install GCC 12 (required by CMakeLists.txt for C++23 support)
RUN apt-get install -y \
    gcc-12 \
    g++-12 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100

# Install core development libraries
RUN apt-get install -y \
    libboost-all-dev \
    libyaml-cpp-dev \
    libjemalloc-dev \
    libgoogle-perftools-dev \
    libgtest-dev \
    libgmock-dev \
    googletest \
    libaio-dev \
    libssl-dev \
    libffi-dev \
    libprotobuf-dev \
    librocksdb-dev \
    && rm -rf /var/lib/apt/lists/*

# Install system utilities
RUN apt-get update && apt-get install -y \
    silversearcher-ag \
    numactl \
    cgroup-tools \
    net-tools \
    strace \
    sshpass \
    ifmetric \
    openssh-server \
    gh \
    && rm -rf /var/lib/apt/lists/*

# Install NUMA and networking libraries
RUN apt-get update && apt-get install -y \
    libnuma-dev \
    libibverbs-dev \
    librdmacm-dev \
    libgflags-dev \
    libnl-3-dev \
    libnl-route-3-dev \
    libsystemd-dev \
    libdpdk-dev \
    libpmem-dev \
    rdma-core \
    && rm -rf /var/lib/apt/lists/*

# Install SoftRoCE (RXE) for RDMA testing in Docker
# Note: SoftRoCE requires kernel modules which may not be available in Docker
# This is a placeholder - actual SoftRoCE setup requires privileged mode
RUN apt-get update && apt-get install -y \
    rdma-core \
    ibverbs-utils \
    rdma-core-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Python and development tools
RUN apt-get update && apt-get install -y \
    python2 \
    python3 \
    python3-dev \
    python3-pip \
    python3-wheel \
    python3-setuptools \
    cython3 \
    python3-docutils \
    pandoc \
    && ln -sf /usr/bin/python3 /usr/bin/python \
    && rm -rf /var/lib/apt/lists/*

# Install debugging and profiling tools
RUN apt-get update && apt-get install -y \
    valgrind \
    gdb \
    htop \
    tree \
    vim \
    nano \
    && rm -rf /var/lib/apt/lists/*

# Install LLVM for rusty-cpp-checker
RUN apt-get update && apt-get install -y \
    llvm-14-dev \
    libclang-14-dev \
    libz3-dev \
    cargo \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Copy requirements.txt from project root and install Python dependencies
COPY requirements.txt /workspace/requirements.txt
RUN pip3 install -r /workspace/requirements.txt --ignore-installed --force-reinstall || \
    (echo "Some packages failed to install, installing compatible ones..." && \
     pip3 install awscli boto3 botocore colorama docutils ecdsa paramiko pyasn1 pycrypto python-dateutil pyyaml rsa s3transfer six tabulate)

# Install Rust (required for MAKO)
RUN curl -LO https://static.rust-lang.org/dist/rust-1.91.0-x86_64-unknown-linux-gnu.tar.gz \
    && tar xzf rust-1.91.0-x86_64-unknown-linux-gnu.tar.gz \
    && cd rust-1.91.0-x86_64-unknown-linux-gnu \
    && mkdir -p /usr/local/rust \
    && ./install.sh --prefix=/usr/local/rust \
    && cd .. \
    && rm -rf rust-1.91.0-x86_64-unknown-linux-gnu rust-1.91.0-x86_64-unknown-linux-gnu.tar.gz

# Add Rust to PATH
ENV PATH="/usr/local/rust/bin:$PATH"

# Verify Rust installation
RUN rustc --version

# Set environment variables for compilation
ENV CC=gcc-12
ENV CXX=g++-12

# Create a non-root user for better security
RUN useradd -m -s /bin/bash mako && \
    chown -R mako:mako /workspace

# Switch to mako user
USER mako

# Set the default command
CMD ["/bin/bash"]
