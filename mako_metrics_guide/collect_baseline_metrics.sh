#!/bin/bash

# Mako Baseline Metrics Collection Script
# This script runs multiple benchmark configurations and collects baseline metrics

set -e

echo "========================================"
echo "Mako Baseline Metrics Collection"
echo "========================================"
echo ""
NCPUS=$(nproc)
DEFAULT_THREADS=$((NCPUS > 24 ? 24 : NCPUS))
echo "Detected: $NCPUS CPUs"
echo "Will use: $DEFAULT_THREADS threads per shard"
echo ""
echo "This script will run the following benchmarks:"
echo "1. Single shard with replication (60 seconds)"
echo "2. Two shards with replication (70 seconds)"
echo ""
echo "Total estimated time: ~3 minutes"
echo ""
read -p "Press Enter to continue or Ctrl+C to cancel..."

# Create results directory
RESULTS_DIR="baseline_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

echo ""
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Function to run a test and extract metrics
run_and_extract() {
    local test_name=$1
    local test_script=$2
    local log_pattern=$3
    
    echo "========================================"
    echo "Running: $test_name"
    echo "========================================"
    
    # Clean up any previous processes
    pkill -9 dbtest 2>/dev/null || true
    sleep 2
    
    # Run the test
    bash "$test_script"
    
    # Find and extract metrics from log files
    for logfile in $log_pattern; do
        if [ -f "$logfile" ]; then
            echo ""
            echo "Extracting metrics from: $logfile"
            ./extract_metrics.sh "$logfile" > "$RESULTS_DIR/$(basename $logfile .log)_metrics.txt"
            
            # Copy the full log
            cp "$logfile" "$RESULTS_DIR/"
            
            echo "Saved to: $RESULTS_DIR/$(basename $logfile .log)_metrics.txt"
        fi
    done
    
    echo ""
    echo "Test completed: $test_name"
    echo ""
}

# Test 1: Single shard with replication
run_and_extract \
    "Single Shard with Replication" \
    "examples/test_1shard_replication.sh" \
    "test_1shard_replication.sh_shard0-localhost-*.log"

# Test 2: Two shards with replication
run_and_extract \
    "Two Shards with Replication" \
    "examples/test_2shard_replication.sh" \
    "test_2shard_replication.sh_rrr_shard*-localhost.log"

# Create summary report
echo "========================================"
echo "Creating Summary Report"
echo "========================================"

SUMMARY_FILE="$RESULTS_DIR/SUMMARY.txt"

cat > "$SUMMARY_FILE" << EOF
Mako Baseline Metrics Summary
Generated: $(date)
========================================

EOF

# Add metrics from each test
for metrics_file in "$RESULTS_DIR"/*_metrics.txt; do
    if [ -f "$metrics_file" ]; then
        echo "=== $(basename $metrics_file _metrics.txt) ===" >> "$SUMMARY_FILE"
        echo "" >> "$SUMMARY_FILE"
        
        # Extract key metrics
        grep "Aggregate Throughput:" "$metrics_file" >> "$SUMMARY_FILE" 2>/dev/null || echo "Throughput: N/A" >> "$SUMMARY_FILE"
        grep "Average Latency:" "$metrics_file" >> "$SUMMARY_FILE" 2>/dev/null || echo "Latency: N/A" >> "$SUMMARY_FILE"
        grep "Aggregate Abort Rate:" "$metrics_file" >> "$SUMMARY_FILE" 2>/dev/null || echo "Abort Rate: N/A" >> "$SUMMARY_FILE"
        grep "NewOrder_remote_abort_ratio:" "$metrics_file" >> "$SUMMARY_FILE" 2>/dev/null || true
        
        echo "" >> "$SUMMARY_FILE"
    fi
done

echo ""
echo "========================================"
echo "Baseline Collection Complete!"
echo "========================================"
echo ""
echo "Results saved to: $RESULTS_DIR"
echo ""
echo "Summary:"
cat "$SUMMARY_FILE"
echo ""
echo "Full metrics available in: $RESULTS_DIR/*_metrics.txt"
echo "Full logs available in: $RESULTS_DIR/*.log"
echo ""

# Create a CSV file for easy import to spreadsheet
CSV_FILE="$RESULTS_DIR/metrics.csv"
echo "Configuration,Throughput (ops/sec),Latency (ms),Abort Rate (aborts/sec),Remote Abort Ratio (%)" > "$CSV_FILE"

for metrics_file in "$RESULTS_DIR"/*_metrics.txt; do
    if [ -f "$metrics_file" ]; then
        config=$(basename "$metrics_file" _metrics.txt)
        throughput=$(grep "Aggregate Throughput:" "$metrics_file" | awk '{print $3}' || echo "N/A")
        latency=$(grep "Average Latency:" "$metrics_file" | awk '{print $3}' || echo "N/A")
        abort_rate=$(grep "Aggregate Abort Rate:" "$metrics_file" | awk '{print $3}' || echo "N/A")
        remote_abort=$(grep "NewOrder_remote_abort_ratio:" "$metrics_file" | awk '{print $2}' | sed 's/%//' || echo "N/A")
        
        echo "$config,$throughput,$latency,$abort_rate,$remote_abort" >> "$CSV_FILE"
    fi
done

echo "CSV file created: $CSV_FILE"
echo "You can import this into Excel or Google Sheets for analysis"
echo ""
