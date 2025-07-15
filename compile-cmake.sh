rm -rf ./out-perf.masstree/*
rm -rf ./src/warbler/out-perf.masstree/*
rm src/warbler/masstree/config.h
cd build
rm -rf CMakeFiles cmake_install.cmake CMakeCache.txt


# 3 shards without replication
shards=3
cmake ..  -DPAXOS_LIB_ENABLED=1  \
          -DMICRO_BENCHMARK=0  SHARDS=$shards \
          -DMERGE_KEYS_GROUPS=$shards \
    
make -j32 dbtest VERBOSE=1
make -j32 basic VERBOSE=1
make -j32 paxos_async_commit_test VERBOSE=1

# rm nfs_sync_127.0.0.1_6001_load_phase_*
# bash bash/shard.sh 3 0 1 30 localhost
# bash bash/shard.sh 3 1 1 30 localhost 
# bash bash/shard.sh 3 2 1 30 localhost 