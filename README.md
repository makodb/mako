## Warbler (OSDI'25)

### Installation
1. dpdk
2. eRPC 
3. masstree
4. paxos(janus)

### Testing compilation
```
make clean
shards=4
MODE=perf make -j32 dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 \
                           MICRO_BENCHMARK=0  TRACKING_LATENCY=0 SHARDS=$shards \
                           MERGE_KEYS_GROUPS=$shards SPANNER=0 MEGA_BENCHMARK=0 \
                           FAIL_NEW_VERSION=1 TRACKING_ROLLBACK=0
```