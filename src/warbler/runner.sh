#!/bin/bash
# https://thorsteinssonh.github.io/bash_test_tools/
source ~/.bash_test_tools
SCRIPT=`realpath $0`
SCRIPTPATH=`dirname $SCRIPT`

WORK="$SCRIPTPATH"

function setup
{
  echo "setup"
  bash batch_srolis.sh kill
  make clean
  make paxos
  make simpleTransaction
  MODE=perf make -j dbtest
  make simpleShards
}

function test_simple_transaction
{
  run "bash scripts/run_simple_transaction.sh"
  assert_equal "$?" "0"
  assert_terminated_normally
  assert_no_error
}

function test_runnable
{
    # 1 shard
    run "./shard0.sh 1 0 4 5"
    assert_equal "$?" "0"
    assert_terminated_normally
    assert_no_error

    # 2 shards
    ./shard0.sh 2 0 4 5 &
    ./shard1.sh 2 1 4 5 &
    sleep 20
    assert_terminated_normally
    assert_no_error
    run "grep agg_throughput: $WORK/results/nshard2-trd4-sIdx0-localhost.log"
    assert_success
    run "grep agg_throughput: $WORK/results/nshard2-trd4-sIdx1-localhost.log"
    assert_success
}

function test_simple_shards
{
    run "bash scripts/run_simple_shards.sh"
    assert_equal "$?" "0"
    assert_terminated_normally
    assert_no_error
}

testrunner
