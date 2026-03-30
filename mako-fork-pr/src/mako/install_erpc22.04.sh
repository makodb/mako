sudo apt update
# rdma-core
cd ~
git clone https://github.com/linux-rdma/rdma-core
cd rdma-core
bash build.sh
cmake .
sudo make install -j10

# This is for Ubuntu22.04
export RTE_SDK=/home/rolis/dpdk-21.11
export DPDK_LIB_DIR=/home/rolis/dpdk-21.11/build/install/usr/local/lib/x86_64-linux-gnu
export PKG_CONFIG_PATH=${DPDK_LIB_DIR}/pkgconfig
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
export LD_LIBRARY_PATH=/home/rolis/dpdk-21.11/build/lib/:/home/rolis/dpdk-21.11/build/install/usr/local/lib/x86_64-linux-gnu:/usr/local/lib
sudo ldconfig
ldconfig -p | grep librte_ethdev

export LD_PRELOAD=/usr/local/lib/libmemcached.so.11:$LD_PRELOAD
export PATH=$PATH:/home/rolis/janus/src/rrr/pylib/:/home/rolis/janus/src

git clone --depth 1 --branch 'v21.11' https://github.com/DPDK/dpdk.git "${RTE_SDK}"
cd "${RTE_SDK}"
meson build -Dexamples='' -Denable_kmods=false -Dtests=false -Ddisable_drivers='raw/*,crypto/*,baseband/*,dma/*'
cd build/
DESTDIR="${RTE_SDK}/build/install" ninja install

cd ~
git clone https://github.com/shenweihai1/eRPC.git
cd eRPC
cp CMakeLists22.04.txt CMakeLists.txt

# for all logs
cmake . -DTRANSPORT=dpdk -DAZURE=on -DPERF=ON -DLOG_LEVEL=cc
make -j4
# build the latency application for experiments
make latency

