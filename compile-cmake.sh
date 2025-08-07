rm -rf ./out-perf.masstree/*
rm -rf ./src/mako/out-perf.masstree/*
#rm src/mako/masstree/config.h
cd build
rm -rf CMakeFiles cmake_install.cmake CMakeCache.txt

# 3 shards without replication
# shards=3
# cmake ..  -DPAXOS_LIB_ENABLED=0  \
#           -DMICRO_BENCHMARK=0 -DSHARDS=$shards

# sudo rm nfs_sync_127.0.0.1_6001_load_phase_*
# bash bash/shard.sh 3 0 6 30 localhost
# bash bash/shard.sh 3 1 6 30 localhost 
# bash bash/shard.sh 3 2 6 30 localhost 


# 3 shards replication
shards=3
cmake ..  -DPAXOS_LIB_ENABLED=1  \
          -DMICRO_BENCHMARK=0 -DSHARDS=$shards

make -j32 dbtest VERBOSE=1
make -j32 basic VERBOSE=1
make -j32 paxos_async_commit_test VERBOSE=1
make -j32 erpc_client VERBOSE=1
make -j32 erpc_server VERBOSE=1