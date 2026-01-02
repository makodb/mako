#include "replication_helper.h"
#include <rusty/cell.hpp>
#include <stdexcept>
#include <iostream>

namespace janus {

// Global replication type with interior mutability
// @safe - Using rusty::Cell for thread-safe interior mutability of Copy type.
// The replication type is set once during initialization and read many times,
// making Cell an appropriate choice (no runtime borrow checking overhead).
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};

// @safe - Read-only access through Cell::get()
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @safe - Mutation through Cell::set()
void set_replication_type(ReplicationType type) {
    g_replication_type.set(type);
    // @unsafe { std::cerr output is not borrow-checked }
    std::cerr << "Replication type set to: " << replication_type_to_string(type) << std::endl;
}

// @safe - String parsing with validation, delegates to set_replication_type
// @unsafe { std::string comparison operators are not borrow-checked }
void set_replication_type_from_string(const std::string& type_str) {
    if (type_str == "paxos" || type_str == "PAXOS") {
        set_replication_type(ReplicationType::PAXOS);
    } else if (type_str == "raft" || type_str == "RAFT") {
        set_replication_type(ReplicationType::RAFT);
    } else {
        throw std::runtime_error("Invalid replication type: " + type_str +
                                 ". Must be 'paxos' or 'raft'");
    }
}

// @safe - Pure function, no side effects
const char* replication_type_to_string(ReplicationType type) {
    switch (type) {
        case ReplicationType::PAXOS: return "paxos";
        case ReplicationType::RAFT: return "raft";
        default: return "unknown";
    }
}

}  // namespace janus

// ============================================================================
// Dispatch Macros
// @safe - These macros dispatch to the appropriate implementation based on
// the runtime replication type. No ownership transfer occurs; arguments are
// forwarded by value or reference as declared in the function signatures.
// ============================================================================

#define DISPATCH_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            return raft_impl::func(__VA_ARGS__); \
        } else { \
            return paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)

#define DISPATCH_VOID_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            raft_impl::func(__VA_ARGS__); \
        } else { \
            paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)

// ============================================================================
// Dispatch Functions
// @unsafe - The implementation functions (paxos_impl/raft_impl) are not
// borrow-checked since they interface with legacy C++ code and third-party
// libraries. The dispatch layer itself is safe but delegates to unsafe code.
// ============================================================================

std::vector<std::string> setup(int argc, char* argv[]) {
    DISPATCH_RAFT_OR_PAXOS(setup, argc, argv);  // @unsafe
}

int setup2(int action, int shardIndex) {
    DISPATCH_RAFT_OR_PAXOS(setup2, action, shardIndex);  // @unsafe
}

std::map<std::string, std::string> getHosts(std::string s) {
    DISPATCH_RAFT_OR_PAXOS(getHosts, s);  // @unsafe
}

int get_outstanding_logs(uint32_t par_id) {
    DISPATCH_RAFT_OR_PAXOS(get_outstanding_logs, par_id);  // @unsafe
}

int shutdown_paxos() {
    DISPATCH_RAFT_OR_PAXOS(shutdown_paxos);  // @unsafe
}

void microbench_paxos() {
    DISPATCH_VOID_RAFT_OR_PAXOS(microbench_paxos);  // @unsafe
}

void register_for_follower(std::function<void(const char*, int)> cb, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_follower, cb, par_id);  // @unsafe
}

void register_for_follower_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_follower_par_id, cb, par_id);  // @unsafe
}

void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_follower_par_id_return, cb, par_id);  // @unsafe
}

void register_for_leader(std::function<void(const char*, int)> cb, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_leader, cb, par_id);  // @unsafe
}

void register_leader_election_callback(std::function<void(int)> cb) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_leader_election_callback, cb);  // @unsafe
}

void register_for_leader_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_leader_par_id, cb, par_id);  // @unsafe
}

void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(register_for_leader_par_id_return, cb, par_id);  // @unsafe
}

void submit(const char* data, int len, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(submit, data, len, par_id);  // @unsafe
}

void add_log(const char* data, int len, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(add_log, data, len, par_id);  // @unsafe
}

void add_log_without_queue(const char* data, int len, uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(add_log_without_queue, data, len, par_id);  // @unsafe
}

void add_log_to_nc(const char* data, int len, uint32_t par_id, int flag) {
    DISPATCH_VOID_RAFT_OR_PAXOS(add_log_to_nc, data, len, par_id, flag);  // @unsafe
}

void wait_for_submit(uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(wait_for_submit, par_id);  // @unsafe
}

void microbench_paxos_queue() {
    DISPATCH_VOID_RAFT_OR_PAXOS(microbench_paxos_queue);  // @unsafe
}

void pre_shutdown_step() {
    DISPATCH_VOID_RAFT_OR_PAXOS(pre_shutdown_step);  // @unsafe
}

int get_epoch() {
    DISPATCH_RAFT_OR_PAXOS(get_epoch);  // @unsafe
}

void set_epoch(int epoch) {
    DISPATCH_VOID_RAFT_OR_PAXOS(set_epoch, epoch);  // @unsafe
}

void upgrade_p1_to_leader() {
    DISPATCH_VOID_RAFT_OR_PAXOS(upgrade_p1_to_leader);  // @unsafe
}

void worker_info_stats(size_t s) {
    DISPATCH_VOID_RAFT_OR_PAXOS(worker_info_stats, s);  // @unsafe
}

void nc_setup_server(int port, std::string host) {
    DISPATCH_VOID_RAFT_OR_PAXOS(nc_setup_server, port, host);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_new_order_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_new_order_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_payment_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_payment_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_delivery_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_delivery_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_order_status_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_order_status_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_stock_level_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_stock_level_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_read_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_read_requests, i);  // @unsafe
}

std::vector<std::vector<int>>* nc_get_rmw_requests(int i) {
    DISPATCH_RAFT_OR_PAXOS(nc_get_rmw_requests, i);  // @unsafe
}

// Raft-specific function - no-op for Paxos
void set_preferred_leader(int site_id) {
    if (janus::is_using_raft()) {
        raft_impl::set_preferred_leader(site_id);  // @unsafe
    }
    // No-op for Paxos
}
