#!/bin/bash

# Script to run Raft 2-shard replication test 10 times and collect replay_batch statistics
# This helps us understand the variance in follower replication performance across multiple shards

OUTPUT_LOG="raft_2shard_batch_variance_$(date +%Y%m%d_%H%M%S).log"

echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Raft 2-Shard Batch Variance Test" | tee -a "$OUTPUT_LOG"
echo "Date: $(date)" | tee -a "$OUTPUT_LOG"
echo "Test: 2-shard Raft with 3 replicas per shard" | tee -a "$OUTPUT_LOG"
echo "Duration: 60 seconds per run" | tee -a "$OUTPUT_LOG"
echo "Runs: 10" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "" | tee -a "$OUTPUT_LOG"

# Arrays to store results for both shards
declare -a shard0_replay_p1
declare -a shard0_replay_p2
declare -a shard0_throughput
declare -a shard1_replay_p1
declare -a shard1_replay_p2
declare -a shard1_throughput
declare -a timestamps

for i in {1..10}; do
    echo "=========================================" | tee -a "$OUTPUT_LOG"
    echo "Run $i/10 - $(date)" | tee -a "$OUTPUT_LOG"
    echo "=========================================" | tee -a "$OUTPUT_LOG"

    # Run the test
    ./ci/ci_mako_raft.sh shard2ReplicationRaft > /tmp/raft_2shard_run_$i.log 2>&1

    # Extract metrics for Shard 0
    replay0_p1=$(grep "replay_batch:" shard0-p1.log 2>/dev/null | tail -1 | sed 's/.*replay_batch://' | awk '{print $1}')
    replay0_p2=$(grep "replay_batch:" shard0-p2.log 2>/dev/null | tail -1 | sed 's/.*replay_batch://' | awk '{print $1}')
    throughput0=$(grep "agg_persist_throughput:" shard0-localhost.log 2>/dev/null | tail -1 | awk '{print $2}')

    # Extract metrics for Shard 1
    replay1_p1=$(grep "replay_batch:" shard1-p1.log 2>/dev/null | tail -1 | sed 's/.*replay_batch://' | awk '{print $1}')
    replay1_p2=$(grep "replay_batch:" shard1-p2.log 2>/dev/null | tail -1 | sed 's/.*replay_batch://' | awk '{print $1}')
    throughput1=$(grep "agg_persist_throughput:" shard1-localhost.log 2>/dev/null | tail -1 | awk '{print $2}')

    # Handle missing values
    [ -z "$replay0_p1" ] && replay0_p1="ERROR"
    [ -z "$replay0_p2" ] && replay0_p2="ERROR"
    [ -z "$throughput0" ] && throughput0="ERROR"
    [ -z "$replay1_p1" ] && replay1_p1="ERROR"
    [ -z "$replay1_p2" ] && replay1_p2="ERROR"
    [ -z "$throughput1" ] && throughput1="ERROR"

    # Store results
    shard0_replay_p1[$i]=$replay0_p1
    shard0_replay_p2[$i]=$replay0_p2
    shard0_throughput[$i]=$throughput0
    shard1_replay_p1[$i]=$replay1_p1
    shard1_replay_p2[$i]=$replay1_p2
    shard1_throughput[$i]=$throughput1
    timestamps[$i]=$(date +%H:%M:%S)

    echo "  Timestamp: ${timestamps[$i]}" | tee -a "$OUTPUT_LOG"
    echo "" | tee -a "$OUTPUT_LOG"
    echo "  Shard 0:" | tee -a "$OUTPUT_LOG"
    echo "    Throughput: $throughput0 ops/sec" | tee -a "$OUTPUT_LOG"
    echo "    Replay p1:  $replay0_p1 batches" | tee -a "$OUTPUT_LOG"
    echo "    Replay p2:  $replay0_p2 batches" | tee -a "$OUTPUT_LOG"
    echo "" | tee -a "$OUTPUT_LOG"
    echo "  Shard 1:" | tee -a "$OUTPUT_LOG"
    echo "    Throughput: $throughput1 ops/sec" | tee -a "$OUTPUT_LOG"
    echo "    Replay p1:  $replay1_p1 batches" | tee -a "$OUTPUT_LOG"
    echo "    Replay p2:  $replay1_p2 batches" | tee -a "$OUTPUT_LOG"
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
echo "" | tee -a "$OUTPUT_LOG"
echo "SHARD 0 RESULTS:" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "Run" "Time" "Replay-p1" "Replay-p2" "Avg-Replay" "Throughput" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "---" "----" "---------" "---------" "----------" "----------" | tee -a "$OUTPUT_LOG"

total_s0_p1=0
total_s0_p2=0
total_s0_throughput=0
valid_s0_runs=0

for i in {1..10}; do
    # Calculate average of p1 and p2
    if [ "${shard0_replay_p1[$i]}" != "ERROR" ] && [ "${shard0_replay_p2[$i]}" != "ERROR" ]; then
        avg_replay=$(( (${shard0_replay_p1[$i]} + ${shard0_replay_p2[$i]}) / 2 ))
    else
        avg_replay="ERROR"
    fi

    printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "$i" "${timestamps[$i]}" "${shard0_replay_p1[$i]}" "${shard0_replay_p2[$i]}" "$avg_replay" "${shard0_throughput[$i]}" | tee -a "$OUTPUT_LOG"

    if [ "${shard0_replay_p1[$i]}" != "ERROR" ]; then
        total_s0_p1=$((total_s0_p1 + ${shard0_replay_p1[$i]}))
        total_s0_p2=$((total_s0_p2 + ${shard0_replay_p2[$i]}))
        total_s0_throughput=$((total_s0_throughput + ${shard0_throughput[$i]%.*}))
        valid_s0_runs=$((valid_s0_runs + 1))
    fi
done

echo "" | tee -a "$OUTPUT_LOG"
echo "SHARD 1 RESULTS:" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "Run" "Time" "Replay-p1" "Replay-p2" "Avg-Replay" "Throughput" | tee -a "$OUTPUT_LOG"
printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "---" "----" "---------" "---------" "----------" "----------" | tee -a "$OUTPUT_LOG"

total_s1_p1=0
total_s1_p2=0
total_s1_throughput=0
valid_s1_runs=0

for i in {1..10}; do
    # Calculate average of p1 and p2
    if [ "${shard1_replay_p1[$i]}" != "ERROR" ] && [ "${shard1_replay_p2[$i]}" != "ERROR" ]; then
        avg_replay=$(( (${shard1_replay_p1[$i]} + ${shard1_replay_p2[$i]}) / 2 ))
    else
        avg_replay="ERROR"
    fi

    printf "%-5s %-10s %-12s %-12s %-12s %-15s\n" "$i" "${timestamps[$i]}" "${shard1_replay_p1[$i]}" "${shard1_replay_p2[$i]}" "$avg_replay" "${shard1_throughput[$i]}" | tee -a "$OUTPUT_LOG"

    if [ "${shard1_replay_p1[$i]}" != "ERROR" ]; then
        total_s1_p1=$((total_s1_p1 + ${shard1_replay_p1[$i]}))
        total_s1_p2=$((total_s1_p2 + ${shard1_replay_p2[$i]}))
        total_s1_throughput=$((total_s1_throughput + ${shard1_throughput[$i]%.*}))
        valid_s1_runs=$((valid_s1_runs + 1))
    fi
done

echo "" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Statistics" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"

# Shard 0 Statistics
if [ $valid_s0_runs -gt 0 ]; then
    echo "" | tee -a "$OUTPUT_LOG"
    echo "SHARD 0:" | tee -a "$OUTPUT_LOG"
    avg_s0_p1=$((total_s0_p1 / valid_s0_runs))
    avg_s0_p2=$((total_s0_p2 / valid_s0_runs))
    avg_s0_replay=$(( (avg_s0_p1 + avg_s0_p2) / 2 ))
    avg_s0_throughput=$((total_s0_throughput / valid_s0_runs))

    echo "  Valid runs: $valid_s0_runs/10" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (p1): $avg_s0_p1" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (p2): $avg_s0_p2" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (overall): $avg_s0_replay" | tee -a "$OUTPUT_LOG"
    echo "  Average throughput: $avg_s0_throughput ops/sec" | tee -a "$OUTPUT_LOG"

    # Calculate min/max for replay_batches (using average of p1 and p2)
    min_batch=999999
    max_batch=0
    for i in {1..10}; do
        if [ "${shard0_replay_p1[$i]}" != "ERROR" ] && [ "${shard0_replay_p2[$i]}" != "ERROR" ]; then
            avg_batch=$(( (${shard0_replay_p1[$i]} + ${shard0_replay_p2[$i]}) / 2 ))
            if [ $avg_batch -lt $min_batch ]; then
                min_batch=$avg_batch
            fi
            if [ $avg_batch -gt $max_batch ]; then
                max_batch=$avg_batch
            fi
        fi
    done

    echo "  Min replay_batch: $min_batch" | tee -a "$OUTPUT_LOG"
    echo "  Max replay_batch: $max_batch" | tee -a "$OUTPUT_LOG"
    echo "  Range: $((max_batch - min_batch))" | tee -a "$OUTPUT_LOG"
    if [ $avg_s0_replay -gt 0 ]; then
        echo "  Variance coefficient: $(awk "BEGIN {printf \"%.2f%%\", ($max_batch - $min_batch) * 100.0 / $avg_s0_replay}")" | tee -a "$OUTPUT_LOG"
    fi
else
    echo "" | tee -a "$OUTPUT_LOG"
    echo "SHARD 0: ERROR - No valid runs completed!" | tee -a "$OUTPUT_LOG"
fi

# Shard 1 Statistics
if [ $valid_s1_runs -gt 0 ]; then
    echo "" | tee -a "$OUTPUT_LOG"
    echo "SHARD 1:" | tee -a "$OUTPUT_LOG"
    avg_s1_p1=$((total_s1_p1 / valid_s1_runs))
    avg_s1_p2=$((total_s1_p2 / valid_s1_runs))
    avg_s1_replay=$(( (avg_s1_p1 + avg_s1_p2) / 2 ))
    avg_s1_throughput=$((total_s1_throughput / valid_s1_runs))

    echo "  Valid runs: $valid_s1_runs/10" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (p1): $avg_s1_p1" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (p2): $avg_s1_p2" | tee -a "$OUTPUT_LOG"
    echo "  Average replay_batch (overall): $avg_s1_replay" | tee -a "$OUTPUT_LOG"
    echo "  Average throughput: $avg_s1_throughput ops/sec" | tee -a "$OUTPUT_LOG"

    # Calculate min/max for replay_batches (using average of p1 and p2)
    min_batch=999999
    max_batch=0
    for i in {1..10}; do
        if [ "${shard1_replay_p1[$i]}" != "ERROR" ] && [ "${shard1_replay_p2[$i]}" != "ERROR" ]; then
            avg_batch=$(( (${shard1_replay_p1[$i]} + ${shard1_replay_p2[$i]}) / 2 ))
            if [ $avg_batch -lt $min_batch ]; then
                min_batch=$avg_batch
            fi
            if [ $avg_batch -gt $max_batch ]; then
                max_batch=$avg_batch
            fi
        fi
    done

    echo "  Min replay_batch: $min_batch" | tee -a "$OUTPUT_LOG"
    echo "  Max replay_batch: $max_batch" | tee -a "$OUTPUT_LOG"
    echo "  Range: $((max_batch - min_batch))" | tee -a "$OUTPUT_LOG"
    if [ $avg_s1_replay -gt 0 ]; then
        echo "  Variance coefficient: $(awk "BEGIN {printf \"%.2f%%\", ($max_batch - $min_batch) * 100.0 / $avg_s1_replay}")" | tee -a "$OUTPUT_LOG"
    fi
else
    echo "" | tee -a "$OUTPUT_LOG"
    echo "SHARD 1: ERROR - No valid runs completed!" | tee -a "$OUTPUT_LOG"
fi

# Combined Statistics
if [ $valid_s0_runs -gt 0 ] && [ $valid_s1_runs -gt 0 ]; then
    echo "" | tee -a "$OUTPUT_LOG"
    echo "COMBINED (Both Shards):" | tee -a "$OUTPUT_LOG"
    combined_avg_replay=$(( (avg_s0_replay + avg_s1_replay) / 2 ))
    combined_avg_throughput=$((avg_s0_throughput + avg_s1_throughput))

    echo "  Average replay_batch (across both shards): $combined_avg_replay" | tee -a "$OUTPUT_LOG"
    echo "  Total average throughput: $combined_avg_throughput ops/sec" | tee -a "$OUTPUT_LOG"
fi

echo "" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"
echo "Test completed at $(date)" | tee -a "$OUTPUT_LOG"
echo "Results saved to: $OUTPUT_LOG" | tee -a "$OUTPUT_LOG"
echo "=========================================" | tee -a "$OUTPUT_LOG"

# Print the log file name for easy reference
echo ""
echo "Full results saved to: $OUTPUT_LOG"
