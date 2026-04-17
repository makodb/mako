# Unified Makefile - Builds both Mako Paxos and Jetpack Raft
# Usage:
#   make              - Build production (Paxos)
#   make mako-raft    - Build Mako with Raft replication layer
#   make raft-test    - Build with Raft testing coroutines enabled
#   make clean        - Clean build artifacts

# Variables
BUILD_DIR = build

PARALLEL_JOBS = $(or $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS))),4)

# TEMPORARY: borrow checking disabled by default to keep build times down.
# Re-enable with `make BORROW_CHECK=ON` (or flip the default back to ON).
BORROW_CHECK ?= OFF
BORROW_FLAG = -DENABLE_BORROW_CHECKING=$(BORROW_CHECK)

.PHONY: all configure build clean rebuild run mako-raft mako-raft-single mako-raft-multi raft-test help test test-verbose test-parallel

all: build

configure:
	cmake -S . -B $(BUILD_DIR) $(BORROW_FLAG)

build: configure
	@echo "Building with $(PARALLEL_JOBS) parallel jobs (borrow check: $(BORROW_CHECK))..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with the Raft helper enabled (single-instance by default)
mako-raft:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON $(BORROW_FLAG)
	@echo "Building Mako with Raft helper using $(PARALLEL_JOBS) parallel jobs (borrow check: $(BORROW_CHECK))..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with single-instance Raft (1 Raft group for all partitions)
mako-raft-single:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON $(BORROW_FLAG)
	@echo "Building Mako with single-instance Raft using $(PARALLEL_JOBS) parallel jobs (borrow check: $(BORROW_CHECK))..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with multi-instance Raft (1 Raft group per partition)
mako-raft-multi:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF $(BORROW_FLAG)
	@echo "Building Mako with multi-instance Raft using $(PARALLEL_JOBS) parallel jobs (borrow check: $(BORROW_CHECK))..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build with Raft testing coroutines enabled
raft-test:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DRAFT_TEST=ON $(BORROW_FLAG)
	@echo "Building Raft test binaries with $(PARALLEL_JOBS) parallel jobs (borrow check: $(BORROW_CHECK))..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

clean:
	rm -rf $(BUILD_DIR) 2>/dev/null || true
	# Remove test files for current user only
	@USERNAME=$${USER:-unknown}; \
	rm -rf /tmp/$${USERNAME}_*;
	# Clean out-perf.masstree
	rm -rf ./out-perf.masstree/*
	# Clean mako out-perf.masstree
	rm -rf ./src/mako/out-perf.masstree/*
	# Clean Masstree configuration
	@echo "Cleaning Masstree configuration..."
	@cd src/mako/masstree && make distclean 2>/dev/null || true
	@rm -f src/mako/masstree/config.h
	@rm -f src/mako/masstree/configure src/mako/masstree/config.status
	@rm -f src/mako/masstree/config.log src/mako/masstree/GNUmakefile
	@rm -f src/mako/masstree/autom4te.cache -rf
	# Clean LZ4 library
	@echo "Cleaning LZ4 library..."
	@cd third-party/lz4 && make clean 2>/dev/null || true
	@rm -f third-party/lz4/liblz4.so third-party/lz4/*.o
	# Clean Rust library
	@echo "Cleaning Rust library..."
	@cd rust-lib && cargo clean 2>/dev/null || true
	# Clean rusty-cpp
	@rm -rf third-party/rusty-cpp/target || true
	# rebuild rpc
	bin/rpcgen --cpp --python src/deptran/rcc_rpc.rpc

rebuild: clean all

run: build
	./$(BUILD_DIR)/dbtest
	./$(BUILD_DIR)/simpleTransaction
	./$(BUILD_DIR)/simpleTransactionRep
	./$(BUILD_DIR)/simplePaxos

# Run tests using ctest
test: build
	@echo "Running tests..."
	@cd $(BUILD_DIR) && ctest --output-on-failure

# Run tests with verbose output
test-verbose: build
	@echo "Running tests with verbose output..."
	@cd $(BUILD_DIR) && ctest --verbose --output-on-failure

# Run tests in parallel
test-parallel: build
	@echo "Running tests in parallel..."
	@cd $(BUILD_DIR) && ctest -j$(PARALLEL_JOBS) --output-on-failure

help:
	@echo "Unified Build System - Mako Paxos + Jetpack Raft"
	@echo ""
	@echo "Build Targets:"
	@echo "  make              - Build production (Paxos) ~2-3 mins"
	@echo "  make mako-raft    - Build Mako with Raft (single-instance, default)"
	@echo "  make mako-raft-single - Build with single-instance Raft (1 group/shard)"
	@echo "  make mako-raft-multi  - Build with multi-instance Raft (1 group/partition)"
	@echo "  make raft-test    - Build with Raft testing coroutines"
	@echo "  make clean        - Clean all build artifacts (REQUIRED when switching backends)"
	@echo "  make rebuild      - Clean and rebuild"
	@echo "  make test         - Run ctest test suite"
	@echo "  make test-verbose - Run tests with verbose output"
	@echo "  make test-parallel- Run tests in parallel"
	@echo ""
	@echo "CI Testing:"
	@echo "  ./ci/ci.sh all              - Run all Paxos CI tests"
	@echo "  ./ci/ci_mako_raft.sh all    - Run all Raft CI tests"
	@echo ""
	@echo "Scalability Benchmarks:"
	@echo "  bash run_scalability_sweep.sh --backend paxos --threads '1 2 4 6 8 12 16' --runs 3"
	@echo "  bash run_scalability_sweep.sh --backend raft-single --threads '1 2 4 6 8 12 16' --runs 3"
	@echo "  bash run_scalability_sweep.sh --backend raft-multi --threads '1 2 4 6 8 12 16' --runs 3"
	@echo "  python3 scripts/process_scalability_results.py --paxos CSV --raft-multi CSV --raft-single CSV"
	@echo ""
	@echo "Config Generation:"
	@echo "  cd config/1leader_2followers && python3 generator.py       - Generate Paxos configs"
	@echo "  cd config/1leader_2followers && python3 raft_generator.py  - Generate Raft configs"
