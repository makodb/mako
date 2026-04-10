# Unified Makefile - Builds both Mako Paxos and Jetpack Raft
# Usage:
#   make              - Build production (Paxos)
#   make mako-raft    - Build Mako with Raft replication layer
#   make raft-test    - Build with Raft testing coroutines enabled
#   make clean        - Clean build artifacts

# Variables
BUILD_DIR = build

PARALLEL_JOBS = $(or $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS))),4)

.PHONY: all configure build clean rebuild run mako-raft mako-raft-single mako-raft-multi raft-test help test test-verbose test-parallel

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	@echo "Building with $(PARALLEL_JOBS) parallel jobs..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with the Raft helper enabled (single-instance by default)
mako-raft:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON
	@echo "Building Mako with Raft helper using $(PARALLEL_JOBS) parallel jobs..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with single-instance Raft (1 Raft group for all partitions)
mako-raft-single:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON
	@echo "Building Mako with single-instance Raft using $(PARALLEL_JOBS) parallel jobs..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build Mako with multi-instance Raft (1 Raft group per partition)
mako-raft-multi:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF
	@echo "Building Mako with multi-instance Raft using $(PARALLEL_JOBS) parallel jobs..."
	cmake --build $(BUILD_DIR) --parallel $(PARALLEL_JOBS)

# Build with Raft testing coroutines enabled
raft-test:
	cmake -S . -B $(BUILD_DIR) -DMAKO_USE_RAFT=ON -DRAFT_TEST=ON
	@echo "Building Raft test binaries with $(PARALLEL_JOBS) parallel jobs..."
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
	@rm -f src/mako/masstree/config.h src/mako/masstree/config.h.in
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
	# Clean rusty-cpp (both host and docker builds)
	@rm -rf third-party/rusty-cpp/target || true
	@rm -rf target-docker || true
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
	@echo "Usage:"
	@echo "  make              - Build production (Paxos) ~2-3 mins"
	@echo "  make mako-raft    - Build Mako with Raft (single-instance, default)"
	@echo "  make mako-raft-single - Build with single-instance Raft (1 group/shard)"
	@echo "  make mako-raft-multi  - Build with multi-instance Raft (1 group/partition)"
	@echo "  make raft-test    - Build with Raft testing coroutines"
	@echo "  make clean        - Clean all build artifacts"
	@echo "  make rebuild      - Clean and rebuild"
	@echo "  make test         - Run ctest test suite"
	@echo "  make test-verbose - Run tests with verbose output"
	@echo "  make test-parallel- Run tests in parallel"
	@echo ""
	@echo "Testing:"
	@echo "  ./ci/ci.sh all                                   - Run all Paxos CI tests"
	@echo "  ./build/deptran_server -f config/3c1s3r3p.yml    - Run Raft server (requires mako-raft)"
