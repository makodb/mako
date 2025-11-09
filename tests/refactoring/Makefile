# Makefile for Refactoring Verification Tests

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2 -g
INCLUDES = -I../../src -I../../src/mako -I../../src/mako/benchmarks/sto -I../../dependencies -I../../third-party

# Source directories
SRC_DIR = ../../src/mako
BENCH_DIR = $(SRC_DIR)/benchmarks/sto

# Test executables
TESTS = test_paxos test_loops stress_memory stress_paxos

# Common object files needed for tests
COMMON_OBJS = 

all: $(TESTS)

# PaxosAcceptor interface test
test_paxos: test_paxos_acceptor.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(COMMON_OBJS)

# Reverse loop bug test
test_loops: test_reverse_loops.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(COMMON_OBJS)

# TransItem memory stress test
stress_memory: stress_transitem_memory.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(COMMON_OBJS)

# Paxos concurrent stress test
stress_paxos: stress_paxos_concurrent.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(COMMON_OBJS)

clean:
	rm -f $(TESTS) *.o

.PHONY: all clean
