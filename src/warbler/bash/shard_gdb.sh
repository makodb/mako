#!/bin/bash
sudo cgdelete -g cpuset:/cpulimit 2>/dev/null || true
sudo cgcreate -t $USER:$USER -a $USER:$USER -g cpuset:/cpulimit
nshard=$1
shard=$2
trd=$3
let up=trd+3
sec=${4:-30}
cluster=${5:-localhost}
sudo cgset -r cpuset.mems=0 cpulimit
sudo cgset -r cpuset.cpus=0-$up cpulimit
mkdir -p results
path=$(pwd)

# gdb --args 
# sudo strace -f -c
#sudo cgexec -g cpuset:cpulimit ./out-perf.masstree/benchmarks/dbtest --verbose --bench tpcc --basedir ./tmp \
sudo gdb --args cgexec -g cpuset:cpulimit ./out-perf.masstree/benchmarks/dbtest --verbose --bench tpcc --basedir ./tmp \
                                      --db-type mbta --num-threads $trd --scale-factor $trd --num-erpc-server 2 \
                                      --shard-index $shard --shard-config $path/config/local-shards$nshard-warehouses$trd.yml \
                                      -F third-party/paxos/config/1leader_2followers/paxos$trd\_shardidx$shard.yml  -F third-party/paxos/config/occ_paxos.yml \
                                      --txn-flags 1 --runtime $sec  -P $cluster --bench-opts \
                                      --new-order-fast-id-gen --retry-aborted-transactions --numa-memory 1G
