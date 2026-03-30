# Makefile for KVStore dynamic library

CXX = g++
CXXFLAGS = -std=c++11 -fPIC -Wall -Wextra -O2
LDFLAGS = -shared -lboost_thread -lboost_system -lboost_chrono

# Library name
LIB_NAME = libkv_store.so
STATIC_LIB = libkv_store.a

# Source files
SOURCES = kv_store.cpp
OBJECTS = $(SOURCES:.cpp=.o)
HEADERS = kv_store.h

# Default target
all: $(LIB_NAME) $(STATIC_LIB)

# Build shared library
$(LIB_NAME): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^

# Build static library
$(STATIC_LIB): $(OBJECTS)
	ar rcs $@ $^

# Compile object files
%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(STATIC_LIB) test_kv

# Install library (optional)
install: $(LIB_NAME)
	sudo cp $(LIB_NAME) /usr/local/lib/
	sudo cp $(HEADERS) /usr/local/include/
	sudo ldconfig

# Test compilation
test: test.cpp $(LIB_NAME)
	$(CXX) -std=c++11 -o test_kv test.cpp -L. -lkv_store -lboost_thread -lboost_system -lboost_chrono

.PHONY: all clean install test
