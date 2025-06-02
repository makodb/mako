export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt update
sudo apt-get --assume-yes install make automake cmake
sudo apt-get --assume-yes install gcc
sudo apt-get --assume-yes install g++
sudo apt-get --assume-yes install boost
sudo apt-get --assume-yes install libboost-all-dev
sudo apt-get --assume-yes install libyaml-cpp-dev libyaml-cpp0.3-dev
sudo apt-get --assume-yes install -y libjemalloc-dev
sudo apt-get --assume-yes install libgoogle-perftools-dev
sudo apt-get --assume-yes install -y libaio-dev
sudo apt-get --assume-yes install build-essential libssl-dev libffi-dev python-dev
sudo apt-get --assume-yes install silversearcher-ag
sudo apt-get --assume-yes install numactl
sudo apt-get --assume-yes install autoconf
sudo apt-get --assume-yes install -y cgroup-tools
sudo apt-get --assume-yes install net-tools
sudo apt-get --assume-yes install -y pkg-config
sudo apt-get --assume-yes install -y strace
sudo apt-get --assume-yes install -y libprotobuf-dev
sudo apt-get --assume-yes install libyaml-cpp-dev
sudo apt-get --assume-yes install python2
sudo apt-get --assume-yes install sshpass

sudo apt-get --assume-yes install build-essential cmake gcc libudev-dev libnl-3-dev  
sudo apt-get --assume-yes install libnl-route-3-dev ninja-build pkg-config valgrind python3-dev
sudo apt-get --assume-yes install libnuma-dev libibverbs-dev libgflags-dev numactl
sudo apt-get --assume-yes install cython3 python3-docutils pandoc make cmake g++ gcc
sudo apt-get --assume-yes install libjemalloc-dev libpmem-dev net-tools ifmetric
sudo apt-get --assume-yes install python3 python3-pip
sudo ln -s /usr/bin/python3 /usr/bin/python
sudo apt-get --assume-yes install libnuma-dev
sudo apt-get --assume-yes install libsystemd-dev
sudo apt-get --assume-yes install meson
sudo apt-get --assume-yes install libpmem-dev

sudo apt-get --assume-yes install build-essential cmake gcc libudev-dev libnl-3-dev libnl-route-3-dev 
sudo apt-get --assume-yes install ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc

sudo add-apt-repository ppa:canonical-server/server-backports -y
sudo apt-get install -y dpdk

# cd ~
# wget https://raw.githubusercontent.com/thorsteinssonh/bash_test_tools/master/bash_test_tools
# sudo cp bash_test_tools ~/.bash_test_tools