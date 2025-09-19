

# Variables
BUILD_DIR = build

.PHONY: all configure build clean rebuild run

all: build

configure:
	cmake -S . -B $(BUILD_DIR) 

build: configure
	cmake --build $(BUILD_DIR) --parallel  

clean:
	rm -rf $(BUILD_DIR)
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




rebuild: clean all

run: build
	./$(BUILD_DIR)/dbtest
	./$(BUILD_DIR)/simpleTransction
	./$(BUILD_DIR)/simpleTransctionRep
	./$(BUILD_DIR)/simplePaxos



