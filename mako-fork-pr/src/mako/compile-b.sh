cd ~/mako
make clean
shards=$1
MODE=perf make -j32 dbtest PAXOS_LIB_ENABLED=1 DISABLE_MULTI_VERSION=0 \
                           MICRO_BENCHMARK=1 TRACKING_LATENCY=0 SHARDS=$shards \
                           SPANNER=0 MEGA_BENCHMARK=0 MEGA_BENCHMARK_MICRO=1 \
                           FAIL_NEW_VERSION=0 TRACKING_ROLLBACK=0
