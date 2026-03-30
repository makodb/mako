#arr=(1 2 3 4 5 6 7 8 9 10)
#arr=(3 4 5 6)
arr=(10 8 6 4 2 1)
arr=(10)
arr=(2 4 6 8)
#arr=(1 2 3 4 5 6 7 8 9 10)
#arr=(5)
arr=(5)
arr=(10 9 8 7 6 5 4 3 2 1)
arr=(10 8 6 4 2 1)

# tpcc
for shards in "${arr[@]}"; do
    s="2pc_tpcc_no_rep_shard"
    bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"
    sleep 1
    bash batch_op_zoo2.sh only_master "bash ~/mako/compile.sh $shards"
    bash ../batch-runner.sh $shards 0 ${s}_$shards
    sleep 10
    bash batch_op_zoo2.sh only_master "bash ~/mako/count.sh $shards"
done

# micro
# for shards in "${arr[@]}"; do
#     s="2pc_micro_no_rep_shard"
#     bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"
#     sleep 1
#     bash batch_op_zoo2.sh only_master "bash ~/mako/compile-m.sh $shards"
#     bash ../batch-runner.sh $shards 0 ${s}_$shards
#     sleep 10
#     bash batch_op_zoo2.sh only_master "bash ~/mako/count-m.sh $shards"
# done