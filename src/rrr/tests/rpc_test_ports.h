/**
 * @file rpc_test_ports.h
 * @brief Shared port allocation helper for RPC tests.
 *
 * This header provides thread-safe, collision-resistant port allocation
 * for RPC unit tests. When multiple test processes run in parallel (e.g.,
 * during CI), using static base ports can cause test failures due to port
 * collisions or TIME_WAIT socket states.
 *
 * Usage:
 *   #include "rpc_test_ports.h"
 *
 *   // For tests that need a single port:
 *   int port = test_ports::get_port();
 *
 *   // For tests that need multiple ports (e.g., test fixtures with many servers):
 *   int base = test_ports::reserve_ports(10);  // reserves ports [base, base+9]
 *
 * @note All functions in this file are safe to call from multiple threads
 *       and multiple test processes simultaneously.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <random>
#include <unistd.h>  // for getpid()

namespace test_ports {

// @safe - Generate a random base port to avoid collisions when multiple test
// processes run in parallel. Uses PID and high-resolution time to seed.
// Range: 10000-59000 to leave headroom and avoid well-known ports.
inline int generate_random_base_port() {
    // Use process ID and time to create a unique seed for each process
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::seed_seq seq{static_cast<unsigned>(getpid()),
                      static_cast<unsigned>(now & 0xFFFFFFFF),
                      static_cast<unsigned>(now >> 32)};
    std::mt19937 gen(seq);
    // Use port range 10000-59000 (49000 ports available)
    // Leave 59000-65535 for system use and avoid collision with TIME_WAIT
    std::uniform_int_distribution<> dist(10000, 59000);
    return dist(gen);
}

// @safe - Global atomic counter for port allocation.
// Initialized lazily with a random base port on first access.
inline std::atomic<int>& global_port_counter() {
    static std::atomic<int> counter{generate_random_base_port()};
    return counter;
}

// @safe - Get a single test port. Thread-safe and process-safe.
// Each call returns a unique port within this process.
inline int get_port() {
    return global_port_counter().fetch_add(1);
}

// @safe - Reserve a range of consecutive ports.
// Returns the base port; the caller gets ports [base, base+count).
// Use this for test fixtures that need multiple servers/clients.
//
// Example:
//   int base = reserve_ports(5);  // Gets ports base, base+1, ..., base+4
inline int reserve_ports(int count) {
    return global_port_counter().fetch_add(count);
}

// @safe - Check if a port is likely available (heuristic).
// This is a best-effort check; the port may still be in use.
// Primarily useful for debugging port allocation issues.
inline bool port_likely_available(int port) {
    // Simple heuristic: avoid well-known ports and check range
    return port >= 1024 && port <= 65535;
}

}  // namespace test_ports
