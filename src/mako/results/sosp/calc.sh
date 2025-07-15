# for v10
#ratios=(1 2 3 5 10 25 50 70 100)
#for ratio in "${ratios[@]}"; do
#   log="v10-tpcc-geolis-replicated/exp-localhost-v10-10-12-*-$ratio.log"
#   #cat $log| ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
#   #cat $log| ag 'agg_throughput'| ag 'agg_throughput' | wc -l
#   cat $log| ag 'NewOrder_remote_abort_ratio'| awk '{sum += $2} END {print sum/10.0}'
#   #cat $log| ag 'NewOrder_remote_commit_latency'| awk '{sum += $2} END {print sum/10.0}'
#   echo ""
#done 
#scp azureuser@20.98.105.220:/home/azureuser/srolis/results/p1-*.log .
#scp azureuser@20.163.31.46:/home/azureuser/srolis/results/p2-*.log .

# for v3
#threads=(1 4 8 12 16 20 24 28)
#for thr in "${threads[@]}"; do
#   log="v3-10-tpcc-replicated/exp-localhost-v3-10-$thr-*.log"
#   log="exp-localhost-v14-10-$thr-*.log"
#   echo "Threads: $thr"
#   cat $log| ag 'agg_throughput'| ag 'agg_throughput' | wc -l
#   cat $log| ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
#   cat $log| ag 'NewOrder_remote_commit_latency'| awk '{sum += $2} END {print sum/10.0}'
#   echo ""
#done

xs=(0 1 5 10 25 100)
for x in "${xs[@]}"; do
   log="exp-localhost-v14-10-24-*-C$x.log"
   echo "x: $x, log: $log"
   echo 'agg_throughput wc -l:'
   cat $log| ag 'agg_throughput'
   cat $log| ag 'agg_throughput'| ag 'agg_throughput' | wc -l
   echo 'sum(agg_throughput):'
   cat $log| ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
   echo 'sum(NewOrder_remote_abort_ratio)/10.0:'
   cat $log| ag 'NewOrder_remote_abort_ratio'| awk '{sum += $2} END {print sum/10.0}'
   echo 'sum(NewOrder_local_abort_ratio)/10.0:'
   cat $log| ag 'NewOrder_local_abort_ratio'| awk '{sum += $2} END {print sum/10.0}'
   echo 'sum(avg_latency)/10.0:'
   cat $log| ag 'avg_latency'| awk '{sum += $2} END {print sum/10.0}'
   echo 'sum(NewOrder_remote_ratio)/10.0:'
   cat $log| ag 'NewOrder_remote_ratio'| awk '{sum += $2} END {print sum/10.0}'
   echo ""
done

