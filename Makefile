

# Variables
BUILD_DIR = build

.PHONY: all configure build clean rebuild run

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DPAXOS_LIB_ENABLED=0 -DMICRO_BENCHMARK=0 -DSHARDS=1

build: configure
	cmake --build $(BUILD_DIR) --parallel  

clean:
	rm -rf $(BUILD_DIR)
	# Clean out-perf.masstree
	rm -rf ./out-perf.masstree/*
	# Clean mako out-perf.masstree
	rm -rf ./src/mako/out-perf.masstree/*


rebuild: clean all

run: build
	./$(BUILD_DIR)/dbtest



