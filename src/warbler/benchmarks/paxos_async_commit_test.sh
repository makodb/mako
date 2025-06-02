#!/bin/bash
# --------- *********** local testing environment *************** ----------------

paxosCompile() {
    cd third-party/paxos
    rm -rf build
    ./waf configure -p build
}

warblerCompile() {
    # Replication
    # Warbler is on master branch
    # Paxos is on srolis-rep branch
    make clean
    shards=3
    MODE=perf make -j32 dbtest PAXOS_LIB_ENABLED=1 DISABLE_MULTI_VERSION=0 \
                               MICRO_BENCHMARK=1  TRACKING_LATENCY=0 SHARDS=$shards \
                               MERGE_KEYS_GROUPS=$shards SPANNER=0 MEGA_BENCHMARK=0 \
                               FAIL_NEW_VERSION=1 TRACKING_ROLLBACK=0
                        
    # No replication
    make clean
    shards=4
    MODE=perf make -j32 dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 \
                               MICRO_BENCHMARK=0  TRACKING_LATENCY=0 SHARDS=$shards \
                               MERGE_KEYS_GROUPS=$shards SPANNER=0 MEGA_BENCHMARK=0 \
                               FAIL_NEW_VERSION=1 TRACKING_ROLLBACK=0
}

preTasks() {
    rm -rf ./results/*.log
    # bash ./initial.sh
}

functionA1() {
    # 3 shards with replication 
    trd=6
    bash bash/shard.sh 3 0 $trd 30 p1 > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 0 $trd 30 p2 > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 0 $trd 30 localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 0 $trd 30 learner > ./results/learner.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 1 $trd 30 p1 > ./results/follower-p1-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 1 $trd 30 p2 > ./results/follower-p2-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 1 $trd 30 localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 1 $trd 30 learner > ./results/learner-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 2 $trd 30 p1 > ./results/follower-p1-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 2 $trd 30 p2 > ./results/follower-p2-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 2 $trd 30 localhost > ./results/leader-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 3 2 $trd 30 learner > ./results/learner-2.log 2>&1 &
    sleep 0.2
}

functionA2() {
    # 1 shard with replication
    trd=6 
    bash bash/shard.sh 1 0 $trd 30 p1 > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 1 0 $trd 30 p2 > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 1 0 $trd 30 localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 1 0 $trd 30 learner > ./results/learner.log 2>&1 &
    sleep 20 
}

functionA3() {
    # 2 shards with replication
    trd=6
    bash bash/shard.sh 2 0 $trd 30 p1 > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 0 $trd 30 p2 > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 0 $trd 30 localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 0 $trd 30 learner > ./results/learner.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 1 $trd 30 p1 > ./results/follower-p1-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 1 $trd 30 p2 > ./results/follower-p2-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 1 $trd 30 localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 2 1 $trd 30 learner > ./results/learner-1.log 2>&1 &
    sleep 0.2
}

functionA4() {
    # 6 shards with replication
    trd=1
    bash bash/shard.sh 6 0 $trd 30 p1 > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 0 $trd 30 p2 > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 0 $trd 30 localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 0 $trd 30 learner > ./results/learner.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 1 $trd 30 p1 > ./results/follower-p1-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 1 $trd 30 p2 > ./results/follower-p2-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 1 $trd 30 localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 1 $trd 30 learner > ./results/learner-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 2 $trd 30 p1 > ./results/follower-p1-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 2 $trd 30 p2 > ./results/follower-p2-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 2 $trd 30 localhost > ./results/leader-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 2 $trd 30 learner > ./results/learner-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 3 $trd 30 p1 > ./results/follower-p1-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 3 $trd 30 p2 > ./results/follower-p2-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 3 $trd 30 localhost > ./results/leader-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 3 $trd 30 learner > ./results/learner-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 4 $trd 30 p1 > ./results/follower-p1-4.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 4 $trd 30 p2 > ./results/follower-p2-4.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 4 $trd 30 localhost > ./results/leader-4.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 4 $trd 30 learner > ./results/learner-4.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 5 $trd 30 p1 > ./results/follower-p1-5.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 5 $trd 30 p2 > ./results/follower-p2-5.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 5 $trd 30 localhost > ./results/leader-5.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 6 5 $trd 30 learner > ./results/learner-5.log 2>&1 &
    sleep 0.2
}

functionB1() {
    # 4 shards, without replication
    trd=24
    bash bash/shard.sh 4 0 $trd 30 localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 1 $trd 30 localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 2 $trd 30 localhost > ./results/leader-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 3 $trd 30 localhost > ./results/leader-3.log 2>&1 &
}

functionB2() {
    # 10 shards without replication
    trd=1
    t=20
    bash bash/shard.sh 10 0 $trd $t localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 1 $trd $t localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 2 $trd $t localhost > ./results/leader-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 3 $trd $t localhost > ./results/leader-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 4 $trd $t localhost > ./results/leader-4.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 5 $trd $t localhost > ./results/leader-5.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 6 $trd $t localhost > ./results/leader-6.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 7 $trd $t localhost > ./results/leader-7.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 8 $trd $t localhost > ./results/leader-8.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 10 9 $trd $t localhost > ./results/leader-9.log 2>&1 &
    sleep 0.2
}

functionCompileC() {
    compile=$1
    if [ $compile -eq 1 ]; then
      make paxos && make paxos_async_commit_test
      sleep 0.2
    fi
}

functionC1() {
    nthreads=4
    ./out-perf.masstree/benchmarks/paxos_async_commit_test -F third-party/paxos/config/1leader_2followers/paxos$nthreads\_shardidx0.yml  -F third-party/paxos/config/occ_paxos.yml -P p1 --partitions $nthreads > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    ./out-perf.masstree/benchmarks/paxos_async_commit_test -F third-party/paxos/config/1leader_2followers/paxos$nthreads\_shardidx0.yml  -F third-party/paxos/config/occ_paxos.yml -P p2 --partitions $nthreads > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    ./out-perf.masstree/benchmarks/paxos_async_commit_test -F third-party/paxos/config/1leader_2followers/paxos$nthreads\_shardidx0.yml  -F third-party/paxos/config/occ_paxos.yml -P localhost --partitions $nthreads > ./results/leader.log 2>&1 &
    sleep 0.2
    ./out-perf.masstree/benchmarks/paxos_async_commit_test -F third-party/paxos/config/1leader_2followers/paxos$nthreads\_shardidx0.yml  -F third-party/paxos/config/occ_paxos.yml -P learner --partitions $nthreads > ./results/learner.log 2>&1 &
    sleep 5
}

functionKillDC() {
   # mimic the datacente failure
   while ! grep -q "15000 ms" ./results/leader.log; do
       sleep 0.1
   done
   echo "event triggerd, Kill the leader datacenter"
   bash ./bash/kill_leader_datacenter.sh
   sleep 45
   echo "killed"
}

functionKillShard() {
   sleep 20
   while ! grep -q "15000 ms" ./results/leader.log; do
       sleep 0.1
   done
   ps aux |  grep -i localhost | grep -i "shard-index 0" |  awk '{print $2}'  |  xargs sudo kill -9
   sleep 45
   echo "killed"
}

postTasks() {
   echo "postTasks DONE!"
   reset
}

preTasks
#functionA1
#functionA2
functionA3
#functionA4
#functionB1
#functionB2
#functionCompileC
#functionC1
#functionKillDC
#functionKillShard
postTasks