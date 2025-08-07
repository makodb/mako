make clean
shards=4
MODE=perf make VERBOSE=1 -j32 dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 \
                           MICRO_BENCHMARK=0  TRACKING_LATENCY=0 SHARDS=$shards \
                           MEGA_BENCHMARK=0 \
                           FAIL_NEW_VERSION=1 TRACKING_ROLLBACK=0
