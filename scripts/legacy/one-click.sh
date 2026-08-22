#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

repos="depfast"  # repos name, default
workdir="~/code"  # we default put our repos under the root

s1=$( cat "${REPO_ROOT}/ips/ip_s1" )
s2=$( cat "${REPO_ROOT}/ips/ip_s2" )
s3=$( cat "${REPO_ROOT}/ips/ip_s3" )
s4=$( cat "${REPO_ROOT}/ips/ip_s4" )
s5=$( cat "${REPO_ROOT}/ips/ip_s5" )
c1=$( cat "${REPO_ROOT}/ips/ip_c1" )
is_rw=$( cat "${REPO_ROOT}/ips/is_rw" )
TPS="tps: "
servers=(
  $s1
  $s2
  $s3
  $s4
  $s5
)

ONLY_CMD=0
SLOWDOWN_DUR=120
SLOWDOWN_DUR_EXP=160
TUPT_DUR=60
TUPT_DUR_EXP=100

if [ $is_rw -eq 1 ]
then  # for rw
  echo "using rw..."
  SLOW_CONCURRENT_RAFT=340
else
  echo "using tpca"
  SLOW_CONCURRENT_RAFT=200
fi


# trials, by default: 1
# please keep same as the variable in ./data_processing/processing.py
FIGURE5a_TARIALS=3
FIGURE5b_TARIALS=3
ulimit -n 10000
LOG_FILE="${REPO_ROOT}/log.txt"
echo "" > $LOG_FILE

setup () {
    if [ $ONLY_CMD -eq 0 ]
    then
      echo "TRY to kill"
      bash "${SCRIPT_DIR}/batch_op.sh" kill
      #bash ./batch_op.sh init
      sleep 5
    fi
}

build_scp() {
  python3 waf configure -J build
  bash "${SCRIPT_DIR}/batch_op.sh" scp
}

timeout_process() {
  cmd=$1
  waitTime=$2
  rerun=$3
  myPid=$!  echo "[$(date)]START timeout_process $cmd, rerun: $rerun\n" >> $LOG_FILE

  sleep $waitTime
  if kill -0 "$myPid"; then
    # still alive, kill it then re-run it
    kill -9 "$myPid"
    bash "${SCRIPT_DIR}/batch_op.sh" kill
    if [ $rerun -eq 1 ]
    then
       setup
       eval $cmd
       timeout_process "$cmd" $waitTime 0
    fi
  else
    echo "job is done"
  fi
}

# figure5a:
#  1. fail-slow on followers with raft
#  2. no slowdown
#  3. replicas: 3, 5
#  4. fix # of client and then vary # of concurrent
experiment5a() {
    suffix=_$1
    mkdir -p ./figure5a$suffix
    rm -rf ./figure5a$suffix/*

    rm -rf ./results
    # 3 replicas
    if [ $is_rw -eq 1 ]
    then  # for rw
      conc=( 20 40 60 80 100 130 160 190 200 220 260 300 340 380 420 460 500 540 580 )
    else
      conc=( 20 40 60 80 100 130 160 190 200 220 260 300 340 380 420 )
    fi

    for i in "${conc[@]}"
    do
      mkdir results
      cmd="${SCRIPT_DIR}/start-exp.sh testname $TUPT_DUR 0 3 follower 1 $i raft nonlocal &"
      if [ $ONLY_CMD -eq 1 ]
      then
        echo $cmd
      else
	setup
        eval $cmd
        timeout_process "$cmd" $TUPT_DUR_EXP 1
	# if error detected, re-run it
        if ! ag $TPS ./log; then
	  echo "[$(date)]not TPS\n " >> $LOG_FILE
	  setup
          eval $cmd
          timeout_process "$cmd" $TUPT_DUR_EXP 0
	fi
        ag $TPS ./log >> $LOG_FILE
      fi
      mv results ./figure5a$suffix/results_3_$i
      cp -r log ./figure5a$suffix/log_3_$i
    done

    # 5 replicas
    for i in "${conc[@]}"
    do
      mkdir results
      cmd="${SCRIPT_DIR}/start-exp.sh testname $TUPT_DUR 0 5 follower 1 $i raft nonlocal &"
      if [ $ONLY_CMD -eq 1 ]
      then
        echo $cmd
      else
	setup
        eval $cmd
        timeout_process "$cmd" $TUPT_DUR_EXP 1
	# if error detected, re-run it
        if ! ag $TPS ./log; then
	  echo "[$(date)]not TPS\n " >> $LOG_FILE
	  setup
          eval $cmd
          timeout_process "$cmd" $TUPT_DUR_EXP 0
	fi
        ag $TPS ./log >> $LOG_FILE
      fi
      mv results ./figure5a$suffix/results_5_$i
      cp -r log ./figure5a$suffix/log_5_$i
    done
}

# figure5b:
#  1. fail-slow on followers with raft
#  2. with 6 slowdown types
#  3. replicas: 3, 5
experiment5b() {
    suffix=_$1
    mkdir -p ./figure5b$suffix
    rm -rf ./figure5b$suffix/*

    rm -rf ./results
    # 3 replicas
    exp=( 1 2 3 4 5 6 )
    for i in "${exp[@]}"
    do
      mkdir results
      cmd="${SCRIPT_DIR}/start-exp.sh testname $SLOWDOWN_DUR $i 3 follower 1 $SLOW_CONCURRENT_RAFT raft nonlocal &"
      if [ $ONLY_CMD -eq 1 ]
      then
        echo $cmd
      else
	setup
        eval $cmd
        timeout_process "$cmd" $SLOWDOWN_DUR_EXP 1
	# if error detected, re-run it
        if ! ag $TPS ./log; then
	  echo "[$(date)]not TPS\n " >> $LOG_FILE
	  setup
          eval $cmd
          timeout_process "$cmd" $SLOWDOWN_DUR_EXP 0
	fi
        ag $TPS ./log >> $LOG_FILE
      fi
      mv results ./figure5b$suffix/results_3_$i
      cp -r log ./figure5b$suffix/log_3_$i
    done

    # 5 replicas
    for i in "${exp[@]}"
    do
      mkdir results
      cmd="${SCRIPT_DIR}/start-exp.sh testname $SLOWDOWN_DUR $i 5 follower 1 $SLOW_CONCURRENT_RAFT raft nonlocal &"
      if [ $ONLY_CMD -eq 1 ]
      then
        echo $cmd
      else
	setup
        eval $cmd
        timeout_process "$cmd" $SLOWDOWN_DUR_EXP 1
	# if error detected, re-run it
        if ! ag $TPS ./log; then
	  echo "[$(date)]not TPS\n " >> $LOG_FILE
	  setup
          eval $cmd
          timeout_process "$cmd" $SLOWDOWN_DUR_EXP 0
	fi
        ag $TPS ./log >> $LOG_FILE
      fi
      mv results ./figure5b$suffix/results_5_$i
      cp -r log ./figure5b$suffix/log_5_$i
    done
}

setup
if [ $ONLY_CMD -eq 0 ]
then
  build_scp
fi

for (( c=1; c<=$FIGURE5a_TARIALS; c++ )); do
  experiment5a $c
  echo -e "experiment-5a\n"
done

for (( c=1; c<=$FIGURE5b_TARIALS; c++ )); do
  experiment5b $c
  echo -e "experiment-5b\n"
done

# draw figures
if [ $ONLY_CMD -eq 0 ]
then
  bash draw_figure.sh
fi
