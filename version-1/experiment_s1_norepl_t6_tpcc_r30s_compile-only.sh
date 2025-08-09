#!/bin/bash
#
# Distributed System Experiment Script
# Generated on: 2025-08-08 14:56:50
#
# Experiment Configuration:
#   - Shards: 1
#   - Replication: Disabled
#   - Benchmark: TPC-C
#   - Threads per shard: 6
#   - Runtime: 30 seconds
#   - Mode: Dry run
#
# This script performs the following steps:
# 1. Compilation: Clean build directory and compile with CMake
# 2. Execution: Run distributed benchmark across configured shards
# 3. Results: Log outputs to ./results/ directory
#
# Usage: ./experiment_s1_norepl_t6_tpcc_r30s_compile-only.sh
#

# === COMPILATION ===
# Clean out-perf.masstree
rm -rf ./out-perf.masstree/*
# Clean mako out-perf.masstree
rm -rf ./src/mako/out-perf.masstree/*

# mkdir build directory
mkdir -p build
# Change to build directory
cd build
# Clean CMake cache
rm -rf CMakeFiles cmake_install.cmake CMakeCache.txt

# Configure with CMake
cmake .. -DPAXOS_LIB_ENABLED=1 -DMICRO_BENCHMARK=0 -DSHARDS=1

# Build dbtest
make -j32  VERBOSE=1

# Return to root directory
cd ..