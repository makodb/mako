source bash/util.sh

COMMAND_FILE="./results/commands.txt"

# cd masstree; ./bootstrap.sh; ./configure  CC="cc" CXX="g++" --enable-max-key-len=1024 --disable-assertions --disable-invariants --disable-preconditions --with-malloc=jemalloc
setup() {
  #bash bash/op.sh kill
  mkdir -p results
  echo -e "\n----->>> start to run \n" >> $COMMAND_FILE
}

command() {
  cmd=$1
  nohup=$2
  echo $cmd >> $COMMAND_FILE
  if [[ $nohup == 1 ]];
  then
    eval $cmd &
  else
    eval $cmd
  fi
}

# -----------------------------------------------------------------------------------------------------------------------------------------
# -- basic experiment: one shard + no replication
# non multi-version based
experiment0_0() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=1 MICRO_BENCHMARK=0
  start=1
  end=12
  #for (( trd=$start; trd<=$end; trd++ ))
    needToRunTrds=(1 4 8 12 16 20 24)
    for trd in "${needToRunTrds[@]}"
do
    LOGFILE="./results/exp0_0_$trd.log"
    ps aux  |  grep -i dbtest  |  awk '{print $2}'  |  xargs sudo kill -9
	command "bash bash/shard.sh 1 0 $trd  > $LOGFILE 2>&1" 0
done
}

experiment0_1() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=1 MICRO_BENCHMARK=1
  start=1
  end=12
  #for (( trd=$start; trd<=$end; trd++ ))
    needToRunTrds=(1 4 8 12 16 20 24)
    for trd in "${needToRunTrds[@]}"
do
    LOGFILE="./results/exp0_1_$trd.log"
    ps aux  |  grep -i dbtest  |  awk '{print $2}'  |  xargs sudo kill -9
 	  command "bash bash/shard.sh 1 0 $trd  > $LOGFILE 2>&1" 0
done
}
# -----------------------------------------------------------------------------------------------------------------------------------------
# -- basic experiment: one shard + no replication
# multi-version based
experiment1_0() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 MICRO_BENCHMARK=0
  start=1
  end=12
  #for (( trd=$start; trd<=$end; trd++ ))
    needToRunTrds=(1 4 8 12 16 20 24)
    for trd in "${needToRunTrds[@]}"
do
    LOGFILE="./results/exp1_0_$trd.log"
    ps aux  |  grep -i dbtest  |  awk '{print $2}'  |  xargs sudo kill -9
 	  command "bash bash/shard.sh 1 0 $trd  > $LOGFILE 2>&1" 0
done
}

experiment1_1() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 MICRO_BENCHMARK=1
  start=1
  end=12
  #for (( trd=$start; trd<=$end; trd++ ))
    needToRunTrds=(1 4 8 12 16 20 24)
    for trd in "${needToRunTrds[@]}"
do
    LOGFILE="./results/exp1_1_$trd.log"
    ps aux  |  grep -i dbtest  |  awk '{print $2}'  |  xargs sudo kill -9
 	  command "bash bash/shard.sh 1 0 $trd  > $LOGFILE 2>&1" 0
done
}

# -----------------------------------------------------------------------------------------------------------------------------------------
# -- basic experiment: n shards + no replication
experiment2_0() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 MICRO_BENCHMARK=0
  start=12
  end=12
  nshards=2
  for (( trd=$start; trd<=$end; trd++ ))
do
    bash bash/op.sh kill
    for (( c=0; c<$nshards; c++ ))
    do
      LOGFILE0="./results/exp2_0_nshard${nshards}_sIdx${c}_trd$trd.log"
      command "bash bash/shard.sh $nshards $c $trd > $LOGFILE0 2>&1" 1
      sleep 0.4
    done
    wait_for_jobs
done
}

experiment2_1() {
  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=0 DISABLE_MULTI_VERSION=0 MICRO_BENCHMARK=1
  start=12
  end=12
  nshards=6
  for (( trd=$start; trd<=$end; trd++ ))
do
    bash bash/op.sh kill
    for (( c=0; c<$nshards; c++ ))
    do
      LOGFILE0="./results/exp2_1_nshard${nshards}_sIdx${c}_trd$trd.log"
      command "bash bash/shard.sh $nshards $c $trd > $LOGFILE0 2>&1" 1
      sleep 0.4
    done
    wait_for_jobs
done
}
## -----------------------------------------------------------------------------------------------------------------------------------------
## -- basic experiment: n shards + replication (1 leader + 2 followers)
#experiment2_0() {
#  make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=1 DISABLE_MULTI_VERSION=0
#
#  start=8
#  end=8
#  nshards=3
#  for (( trd=$start; trd<=$end; trd++ ))
#do
#    bash bash/op.sh kill
#
#    for (( c=0; c<$nshards; c++ ))
#    do
#      LOGFILE0="./results/exp2_0_nshard${nshards}_sIdx${c}_trd$trd-p1.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 p1 > $LOGFILE0 2>&1" 1
#
#      LOGFILE1="./results/exp2_0_nshard${nshards}_sIdx${c}_trd$trd-p2.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 p2 > $LOGFILE1 2>&1" 1
#
#      LOGFILE2="./results/exp2_0_nshard${nshards}_sIdx${c}_trd$trd-localhost.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 localhost > $LOGFILE2 2>&1" 1
#
#      LOGFILE3="./results/exp2_0_nshard${nshards}_sIdx${c}_trd$trd-learner.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 learner > $LOGFILE3 2>&1" 1
#
#      sleep 0.2
#    done
#    wait_for_jobs
#done
#}
## -----------------------------------------------------------------------------------------------------------------------------------------
## -- basic experiment: n shards + replication (1 leader + 2 followers) -> kill the leader partitioner:0
#experiment3_0() {
# # make clean && MODE=perf make -j dbtest PAXOS_LIB_ENABLED=1 DISABLE_MULTI_VERSION=0
#
#  start=8
#  end=8
#  nshards=1
#  for (( trd=$start; trd<=$end; trd++ ))
#do
#    bash bash/op.sh kill
#
#    for (( c=0; c<$nshards; c++ ))
#    do
#      LOGFILE0="./results/exp3_0_nshard${nshards}_sIdx${c}_trd$trd-p1.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 p1 > $LOGFILE0 2>&1" 1
#
#      LOGFILE1="./results/exp3_0_nshard${nshards}_sIdx${c}_trd$trd-p2.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 p2 > $LOGFILE1 2>&1" 1
#
#      LOGFILE2="./results/exp3_0_nshard${nshards}_sIdx${c}_trd$trd-localhost.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 localhost > $LOGFILE2 2>&1" 1
#
#      LOGFILE3="./results/exp3_0_nshard${nshards}_sIdx${c}_trd$trd-learner.log"
#      command "bash bash/shard$c.sh $nshards $c $trd 30 learner > $LOGFILE3 2>&1" 1
#
#      sleep 0.2
#    done
#    sleep 20
#    ps aux |  grep -i localhost | grep -i "shard-index 0" |  awk '{print $2}'  |  xargs sudo kill -9
#    wait_for_jobs
#done
#}
setup

experiment0_0
experiment0_1
experiment1_0
experiment1_1
#experiment2_0
#experiment2_1
