
shards=4
make paxos
bash $HOME/mako/compile-coco.sh $shards 

functionRep() {
    # 4 shards with replication
    # 11.8w TPS per shard on zoo-002
    trd=6
    timeout=40
    bash bash/shard.sh 4 0 $trd $timeout p1 > ./results/follower-p1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 0 $trd $timeout p2 > ./results/follower-p2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 0 $trd $timeout localhost > ./results/leader.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 0 $trd $timeout learner > ./results/learner.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 1 $trd $timeout p1 > ./results/follower-p1-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 1 $trd $timeout p2 > ./results/follower-p2-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 1 $trd $timeout localhost > ./results/leader-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 1 $trd $timeout learner > ./results/learner-1.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 2 $trd $timeout p1 > ./results/follower-p1-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 2 $trd $timeout p2 > ./results/follower-p2-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 2 $trd $timeout localhost > ./results/leader-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 2 $trd $timeout learner > ./results/learner-2.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 3 $trd $timeout p1 > ./results/follower-p1-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 3 $trd $timeout p2 > ./results/follower-p2-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 3 $trd $timeout localhost > ./results/leader-3.log 2>&1 &
    sleep 0.2
    bash bash/shard.sh 4 3 $trd $timeout learner > ./results/learner-3.log 2>&1 &
    sleep 0.2
}

functionKillShard() {
   sleep 10
   while ! grep -q "35000 ms" ./results/leader.log; do
       sleep 0.1
   done
   ps aux |  grep -i localhost | grep -i "shard-index 0" |  awk '{print $2}'  |  xargs sudo kill -9
   sleep 15
   echo "killed"
}

rm ./results/*.log
./kill.sh
sleep 2
functionRep

# Mimic shard failure on the leader shard
functionKillShard
reset
