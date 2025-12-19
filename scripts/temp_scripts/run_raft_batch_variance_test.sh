#!/bin/bash

# Script to run Raft replication test 10 times and collect replay_batch statistics
# This helps us understand the variance in follower replication performance

OUTPUT_LOG="raft_batch_variance_$(date +%Y%m%d_%H%M%S).log"

echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Raft Batch Variance Test" | tee -a "$OUTPUT_LOG"
echo "Date: $(date)" | tee -a "$OUTPUT_LOG"
echo "Test: 1-shard Raft with 3 replicas" | tee -a "$OUTPUT_LOG"
echo "Duration: 60 seconds per run" | tee -a "$OUTPUT_LOG"
echo "Runs: 10" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "" | tee -a "$OUTPUT_LOG"

# Arrays to store results
declare -a replay_batches
declare -a throughputs
declare -a timestamps

for i in {1..10}; do
    echo "=========================================" | tee -a "$OUTPUT_LOG"
    echo "Run $i/10 - $(date)" | tee -a "$OUTPUT_LOG"
    echo "=========================================" | tee -a "$OUTPUT_LOG"
    
    # Run the test
    ./ci/ci_mako_raft.sh shard1ReplicationRaft > /tmp/raft_run_$i.log 2>&1
    
    # Extract replay_batch from p1 log
    replay_batch=$(grep "replay_batch:" test_1shard_replication_raft.sh_shard0-p1-6.log 2>/dev/null | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
    
    # Extract throughput from localhost log
    throughput=$(grep "agg_persist_throughput:" test_1shard_replication_raft.sh_shard0-localhost-6.log 2>/dev/null | tail -1 | awk '{print $2}')
    
    if [ -z "$replay_batch" ]; then
        replay_batch="ERROR"
    fi
    
    if [ -z "$throughput" ]; then
        throughput="ERROR"
    fi
    
    # Store results
    replay_batches[$i]=$replay_batch
    throughputs[$i]=$throughput
    timestamps[$i]=$(date +%H:%M:%S)
    
    echo "  Timestamp: ${timestamps[$i]}" | tee -a "$OUTPUT_LOG"
    echo "  Replay Batches: $replay_batch" | tee -a "$OUTPUT_LOG"
    echo "  Throughput: $throughput ops/sec" | tee -a "$OUTPUT_LOG"
    echo "" | tee -a "$OUTPUT_LOG"
    
    # Wait a bit between runs to let system settle
    if [ $i -lt 10 ]; then
        echo "Waiting 5 seconds before next run..." | tee -a "$OUTPUT_LOG"
        sleep 5
    fi
done

echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Summary of All Runs" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-15s %-15s\n" "Run" "Time" "Replay Batches" "Throughput" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-15s %-15s\n" "---" "----" "--------------" "----------" | tee -a "$OUTPUT_LOG"

total_batches=0
total_throughput=0
valid_runs=0

for i in {1..10}; do
    printf "%-5s %-10s %-15s %-15s\n" "$i" "${timestamps[$i]}" "${replay_batches[$i]}" "${throughputs[$i]}" | tee -a "$OUTPUT_LOG"
    
    if [ "${replay_batches[$i]}" != "ERROR" ]; then
        total_batches=$((total_batches + ${replay_batches[$i]}))
        total_throughput=$((total_throughput + ${throughputs[$i]}))
        valid_runs=$((valid_runs + 1))
    fi
done

echo "" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Statistics" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"

if [ $valid_runs -gt 0 ]; then
    avg_batches=$((total_batches / valid_runs))
    avg_throughput=$((total_throughput / valid_runs))
    
    echo "Valid runs: $valid_runs/10" | tee -a "$OUTPUT_LOG"
    echo "Average replay_batch: $avg_batches" | tee -a "$OUTPUT_LOG"
    echo "Average throughput: $avg_throughput ops/sec" | tee -a "$OUTPUT_LOG"
    
    # Calculate min/max for replay_batches
    min_batch=999999
    max_batch=0
    for i in {1..10}; do
        if [ "${replay_batches[$i]}" != "ERROR" ]; then
            batch=${replay_batches[$i]}
            if [ $batch -lt $min_batch ]; then
                min_batch=$batch
            fi
            if [ $batch -gt $max_batch ]; then
                max_batch=$batch
            fi
        fi
    done
    
    echo "Min replay_batch: $min_batch" | tee -a "$OUTPUT_LOG"
    echo "Max replay_batch: $max_batch" | tee -a "$OUTPUT_LOG"
    echo "Range: $((max_batch - min_batch))" | tee -a "$OUTPUT_LOG"
    echo "Variance coefficient: $(awk "BEGIN {printf \"%.2f%%\", ($max_batch - $min_batch) * 100.0 / $avg_batches}")" | tee -a "$OUTPUT_LOG"
else
    echo "ERROR: No valid runs completed!" | tee -a "$OUTPUT_LOG"
fi

echo "" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Test completed at $(date)" | tee -a "$OUTPUT_LOG"
echo "Results saved to: $OUTPUT_LOG" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"

# Print the log file name for easy reference
echo ""
echo "Full results saved to: $OUTPUT_LOG"
