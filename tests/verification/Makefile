# Makefile for Refactoring Verification Tests

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2 -g
INCLUDES = -I../../src -I../../src/mako -I../../src/mako/benchmarks/sto -I../../dependencies -I../../third-party

# Source directories
SRC_DIR = ../../src/mako
BENCH_DIR = $(SRC_DIR)/benchmarks/sto
UNIT_DIR = unit

# Test executables
TESTS = test_paxos test_loops stress_memory stress_paxos

# Unit test executables - STO
UNIT_STO_TESTS = \
	$(UNIT_DIR)/sto/test_hashtable \
	$(UNIT_DIR)/sto/test_vector \
	$(UNIT_DIR)/sto/test_queue \
	$(UNIT_DIR)/sto/test_priority_queue \
	$(UNIT_DIR)/sto/test_rbtree \
	$(UNIT_DIR)/sto/test_transaction_helpers \
	$(UNIT_DIR)/sto/test_transitem

# Unit test executables - Silo
UNIT_SILO_TESTS = \
	$(UNIT_DIR)/silo/test_txn_core \
	$(UNIT_DIR)/silo/test_mbta_wrapper \
	$(UNIT_DIR)/silo/test_utilities

# All unit tests
UNIT_TESTS = $(UNIT_STO_TESTS) $(UNIT_SILO_TESTS)

# Common object files needed for tests
COMMON_OBJS = 

all: $(TESTS)

unit: $(UNIT_TESTS)

all_tests: $(TESTS) $(UNIT_TESTS)

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

# Unit tests - STO
$(UNIT_DIR)/sto/test_hashtable: $(UNIT_DIR)/sto/test_hashtable.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_vector: $(UNIT_DIR)/sto/test_vector.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_queue: $(UNIT_DIR)/sto/test_queue.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_priority_queue: $(UNIT_DIR)/sto/test_priority_queue.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_rbtree: $(UNIT_DIR)/sto/test_rbtree.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_transaction_helpers: $(UNIT_DIR)/sto/test_transaction_helpers.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/sto/test_transitem: $(UNIT_DIR)/sto/test_transitem.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

# Unit tests - Silo
$(UNIT_DIR)/silo/test_txn_core: $(UNIT_DIR)/silo/test_txn_core.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/silo/test_mbta_wrapper: $(UNIT_DIR)/silo/test_mbta_wrapper.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

$(UNIT_DIR)/silo/test_utilities: $(UNIT_DIR)/silo/test_utilities.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clean:
	rm -f $(TESTS) $(UNIT_TESTS) *.o

.PHONY: all unit all_tests clean
