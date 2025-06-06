rm -rf ./out-perf.masstree/*
rm -rf ./src/warbler/out-perf.masstree/*
rm src/warbler/masstree/config.h
cd build
rm -rf CMakeFiles cmake_install.cmake CMakeCache.txt
cmake .. -DDEBUG=ON -DPAXOS_LIB_ENABLED=0 -DDISABLE_MULTI_VERSION=0 \
        -DMICRO_BENCHMARK=0 -DTRACKING_LATENCY=0 -DSHARDS=4 \
        -DMERGE_KEYS_GROUPS=4 -DMEGA_BENCHMARK=0 \
        -DFAIL_NEW_VERSION=1 -DTRACKING_ROLLBACK=0


make -j32 dbtest VERBOSE=1
make -j32 basic VERBOSE=1
make -j32 paxos_async_commit_test VERBOSE=1