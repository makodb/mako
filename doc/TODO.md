## TODOs

1. disk watermark control

2. simple hello world example

3. failure recovery

4. more testcases to verify the values

5. deal with slow / fault single server!

6. not stable! for example simpleTransacitonRep can even not work in some cases!


7. Strange: sometimes ././ci/ci.sh shard1ReplicationSimple would halt

=== Mako Transaction Tests  ===
allocator::Initialize()
  hugepgsize: 2097152
  use MADV_WILLNEED: 1
  mmap() region [0x7f66faae1000, 0x7f673b4e1000)
cpu0 owns [0x7f66fac00000, 0x7f6705800000)
cpu1 owns [0x7f6705800000, 0x7f6710400000)
cpu2 owns [0x7f6710400000, 0x7f671b000000)
cpu3 owns [0x7f671b000000, 0x7f6725c00000)
cpu4 owns [0x7f6725c00000, 0x7f6730800000)
cpu5 owns [0x7f6730800000, 0x7f673b400000)
Database Benchmark:
  pid: 895585
settings:
  num-cpus    : 128
  num-threads : 6
  shardIndex  : 0
  paxos_proc_name  : localhost
  nshards     : 1
  is_micro    : 0
  is_replicated : 1
  var-encode  : yes
  allocator   : jemalloc
system properties:
  tuple_prefetch          : no
  btree_node_prefetch     : yes
new table is createded with name: customer_0, table-id: 1, on shard-server id:0
existing table is createded with name: customer_0, table-id: 1, on shard-server id:0
new table is createded with name: customer_1, table-id: 2, on shard-server id:0
20250912-02:09.15-414974(us) 895585 * ParseOldFormat  (configuration.cc:64): Using old configuration format
20250912-02:09.15-415094(us) 895585 * ParseOldFormat  (configuration.cc:64): Using old configuration format
55:425405 INFOR: eRPC Nexus: Created with management UDP port 31011, hostname 127.0.0.1.
55:476143 INFOR: eRPC Nexus: Created with management UDP port 31012, hostname 127.0.0.1.