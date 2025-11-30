

# Variables
BUILD_DIR = build

.PHONY: all configure build clean rebuild run test test-verbose test-parallel

all: build

configure:
	cmake -S . -B $(BUILD_DIR) 

build: configure
	@echo "Building with $(if $(filter -j%,$(MAKEFLAGS)),$(subst -j,,$(filter -j%,$(MAKEFLAGS))),4) parallel jobs..."
	cmake --build $(BUILD_DIR) --parallel $(if $(filter -j%,$(MAKEFLAGS)),$(subst -j,,$(filter -j%,$(MAKEFLAGS))),4)  

clean:
	rm -rf $(BUILD_DIR) 2>/dev/null || true
	# Remove all test files
	rm -rf /tmp/test_*
	# Remove all disk db
	rm -rf /tmp/rocksdb_*
	rm -rf /tmp/callback_demo_db*
	# rm -rf /tmp/mako_rocksdb*
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
	# Clean rusty-cpp
	@rm -rf third-party/rusty-cpp/target || true
	# rebuild rpc
	bin/rpcgen --cpp --python src/deptran/rcc_rpc.rpc

rebuild: clean all

run: build
	./$(BUILD_DIR)/dbtest
	./$(BUILD_DIR)/simpleTransction
	./$(BUILD_DIR)/simpleTransctionRep
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
	@cd $(BUILD_DIR) && ctest -j$(if $(filter -j%,$(MAKEFLAGS)),$(subst -j,,$(filter -j%,$(MAKEFLAGS))),4) --output-on-failure




