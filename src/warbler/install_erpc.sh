
# rdma-core
cd ~
git clone https://github.com/linux-rdma/rdma-core
cd rdma-core
bash build.sh
cmake .
sudo make install -j10

# dpdk
sudo add-apt-repository ppa:canonical-server/server-backports -y
sudo apt-get install -y dpdk

cd ~
wget http://static.dpdk.org/rel/dpdk-19.11.5.tar.gz
tar -xvf dpdk-19.11.5.tar.gz
cd dpdk-stable-19.11.5
make config T=x86_64-native-linuxapp-gcc
sed -ri 's,(MLX._PMD=)n,\1y,' build/.config
sed -i 's/CONFIG_RTE_LIBRTE_MLX5_PMD=n/CONFIG_RTE_LIBRTE_MLX5_PMD=y/g' config/common_base
sed -i 's/CONFIG_RTE_LIBRTE_MLX4_PMD=n/CONFIG_RTE_LIBRTE_MLX4_PMD=y/g' config/common_base
# it's only required for Ubuntu20.04
sed -i 's/-Werror/-Werror -Wno-error=incompatible-pointer-types -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-fallthrough/g' kernel/linux/igb_uio/Makefile
sed -i 's/-Werror/-Werror -Wno-error=incompatible-pointer-types -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-fallthrough/g' kernel/linux/kni/Makefile
sed -i 's/get_user_pages_remote(tsk, tsk/get_user_pages_remote(tsk/g' kernel/linux/kni/kni_dev.h
sudo make -j32
sudo make install T=x86_64-native-linuxapp-gcc DESTDIR=/usr -j32


cd ~
git clone https://github.com/shenweihai1/eRPC.git
cd eRPC

# for all logs
cmake . -DTRANSPORT=dpdk -DAZURE=on -DPERF=ON 
make -j32
# build the latency application for experiments
make latency