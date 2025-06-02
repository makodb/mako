cd ~/srolis
make clean
shards=$1
# for COCO, we have to use a single timestamp and compress all vectors into a single timestamp
# at the same time, we target the duration to advance the epoch is 10ms.
MODE=perf make -j32 dbtest PAXOS_LIB_ENABLED=1 DISABLE_MULTI_VERSION=0 \
                           MICRO_BENCHMARK=0 TRACKING_LATENCY=0 SHARDS=$shards \
                           MERGE_KEYS_GROUPS=1 SPANNER=0 MEGA_BENCHMARK=0 MEGA_BENCHMARK_MICRO=0\
                           FAIL_NEW_VERSION=1 TRACKING_ROLLBACK=1 COCO=1
