export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt update

# Install GCC 12 (required by CMakeLists.txt for C++23 support)
sudo apt-get --assume-yes install software-properties-common
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get --assume-yes install gcc-12 g++-12
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100

sudo apt-get --assume-yes install make automake cmake
sudo apt-get --assume-yes install boost
sudo apt-get --assume-yes install libboost-all-dev
sudo apt-get --assume-yes install libyaml-cpp-dev libyaml-cpp0.3-dev
sudo apt-get --assume-yes install -y libjemalloc-dev
sudo apt-get --assume-yes install libgoogle-perftools-dev
sudo apt-get --assume-yes install googletest
sudo apt-get install libgtest-dev libgmock-dev -y
sudo apt-get --assume-yes install -y libaio-dev # remove?
sudo apt-get --assume-yes install build-essential libssl-dev libffi-dev python3-dev
sudo apt-get --assume-yes install silversearcher-ag
sudo apt-get --assume-yes install numactl
sudo apt-get --assume-yes install autoconf
sudo apt-get --assume-yes install -y cgroup-tools
sudo apt-get --assume-yes install net-tools
sudo apt-get --assume-yes install -y pkg-config
sudo apt-get --assume-yes install -y strace
sudo apt-get --assume-yes install -y libprotobuf-dev
sudo apt-get --assume-yes install libyaml-cpp-dev
sudo apt-get --assume-yes install python2 #remove?
sudo apt-get --assume-yes install sshpass
sudo apt-get --assume-yes install librocksdb-dev

sudo apt-get --assume-yes install build-essential cmake libudev-dev libnl-3-dev  
sudo apt-get --assume-yes install libnl-route-3-dev ninja-build pkg-config valgrind python3-dev
sudo apt-get --assume-yes install libnuma-dev libibverbs-dev libgflags-dev numactl
sudo apt-get --assume-yes install cython3 python3-docutils pandoc make cmake
sudo apt-get --assume-yes install libjemalloc-dev libpmem-dev net-tools ifmetric
sudo apt-get --assume-yes install python3 python3-pip
sudo ln -s /usr/bin/python3 /usr/bin/python
sudo apt-get --assume-yes install libnuma-dev
sudo apt-get --assume-yes install libsystemd-dev
sudo apt-get --assume-yes install libdpdk-dev
# sudo apt-get --assume-yes install meson # remove?
sudo apt-get --assume-yes install libpmem-dev

sudo apt-get --assume-yes install build-essential cmake gcc libudev-dev libnl-3-dev libnl-route-3-dev 
sudo apt-get --assume-yes install ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc

sudo apt-get --assume-yes install gh cargo openssh-server
# for rusty-cpp-checker
sudo apt-get --assume-yes install llvm-14-dev libclang-14-dev libz3-dev
