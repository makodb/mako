# Run Microbenchmark with replication in the same DC
# arr=(0 5 10 40 60 90 100)
# arr=(40)
# for ratio in "${arr[@]}"; do
#     bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"
#     bash batch_op_zoo2.sh only_master "cd ~/mako; sed -i 's/static const size_t g_micro_remote_item_pct = [0-9]\+;/static const size_t g_micro_remote_item_pct = $ratio;/' benchmarks/tpcc.cc" 
#     bash batch_op_zoo2.sh only_master "bash ~/mako/compile-a.sh 2"
#     bash ../batch-runner.sh 2 1 samedc_${ratio}
#     sleep 35
#     bash batch_op_zoo2.sh only_master "bash ~/mako/count-a.sh 2 $ratio"
# done

# Run Microbenchmark with replication in the different DC
# arr=(0 5 10 40 60 90 100)
# arr=(10 40 60 90 100)
# arr=(5 10 40 60 90 100)
# arr=()
# for ratio in "${arr[@]}"; do
#    bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"
#    bash batch_op_zoo2.sh only_master "cd ~/mako; sed -i 's/static const size_t g_micro_remote_item_pct = [0-9]\+;/static const size_t g_micro_remote_item_pct = $ratio;/' benchmarks/tpcc.cc" 
#    bash batch_op_zoo2.sh only_master "bash ~/mako/compile-b.sh 2"
#    bash ../batch-runner.sh 2 1 diffdc_${ratio}
#    sleep 35
#    bash batch_op_zoo2.sh only_master "bash ~/mako/count-b.sh $ratio"
# done

# cross-shard within dc on Microbenchmark
arr=(0 5 10 40 60 90 100)
arr=(40 100)
for ratio in "${arr[@]}"; do
    bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"
    bash batch_op_zoo2.sh only_master "cd ~/mako; sed -i 's/static const size_t g_micro_remote_item_pct = [0-9]\+;/static const size_t g_micro_remote_item_pct = $ratio;/' benchmarks/tpcc.cc" 
    bash batch_op_zoo2.sh only_master "bash ~/mako/compile-a.sh 10"
    bash ../batch-runner.sh 10 1 samedc_${ratio}
    sleep 35
    bash batch_op_zoo2.sh only_master "bash ~/mako/count-a.sh 10 $ratio"
done
