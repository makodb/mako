#pragma once

// replication_helper.h - Runtime switching between Paxos and Raft replication
//
// This header provides a unified interface for replication that can be switched
// at runtime using the --replication=paxos|raft command-line flag.
//
// @safe - This module uses rusty::Cell for interior mutability of the global
// replication type, ensuring safe mutation through a const-correct API.

#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <vector>
#include <cstdint>

namespace janus {

// Replication type enum - Copy type suitable for rusty::Cell
// @safe - trivially copyable enum
enum class ReplicationType : int {
    PAXOS = 0,
    RAFT = 1
};

// Get/set the current replication type (default: PAXOS)
// @safe - uses Cell for interior mutability
ReplicationType get_replication_type();
void set_replication_type(ReplicationType type);
void set_replication_type_from_string(const std::string& type_str);
const char* replication_type_to_string(ReplicationType type);

// Check if using specific replication type
// @safe - read-only accessors
inline bool is_using_raft() { return get_replication_type() == ReplicationType::RAFT; }
inline bool is_using_paxos() { return get_replication_type() == ReplicationType::PAXOS; }

}  // namespace janus

// ============================================================================
// Paxos Implementation Namespace Forward Declarations
// ============================================================================
namespace paxos_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
std::map<std::string, std::string> getHosts(std::string);
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char*, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void register_for_leader(std::function<void(const char*, int)>, uint32_t);
void register_leader_election_callback(std::function<void(int)>);
void register_for_leader_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
void add_log_to_nc(const char*, int, uint32_t, int);
void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();
void set_epoch(int epoch);
void upgrade_p1_to_leader();
void worker_info_stats(size_t);
void nc_setup_server(int, std::string);
std::vector<std::vector<int>>* nc_get_new_order_requests(int);
std::vector<std::vector<int>>* nc_get_payment_requests(int);
std::vector<std::vector<int>>* nc_get_delivery_requests(int);
std::vector<std::vector<int>>* nc_get_order_status_requests(int);
std::vector<std::vector<int>>* nc_get_stock_level_requests(int);
std::vector<std::vector<int>>* nc_get_read_requests(int);
std::vector<std::vector<int>>* nc_get_rmw_requests(int);
}  // namespace paxos_impl

// ============================================================================
// Raft Implementation Namespace Forward Declarations
// ============================================================================
namespace raft_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
std::map<std::string, std::string> getHosts(std::string);
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char*, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void register_for_leader(std::function<void(const char*, int)>, uint32_t);
void register_leader_election_callback(std::function<void(int)>);
void register_for_leader_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
void add_log_to_nc(const char*, int, uint32_t, int);
void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();
void set_epoch(int epoch);
void upgrade_p1_to_leader();
void worker_info_stats(size_t);
void nc_setup_server(int, std::string);
std::vector<std::vector<int>>* nc_get_new_order_requests(int);
std::vector<std::vector<int>>* nc_get_payment_requests(int);
std::vector<std::vector<int>>* nc_get_delivery_requests(int);
std::vector<std::vector<int>>* nc_get_order_status_requests(int);
std::vector<std::vector<int>>* nc_get_stock_level_requests(int);
std::vector<std::vector<int>>* nc_get_read_requests(int);
std::vector<std::vector<int>>* nc_get_rmw_requests(int);
void set_preferred_leader(int site_id);
}  // namespace raft_impl

// ============================================================================
// Unified Replication API - Dispatches to appropriate implementation
// ============================================================================
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action = 0, int shardIndex = -1);
std::map<std::string, std::string> getHosts(std::string);
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char*, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void register_for_leader(std::function<void(const char*, int)>, uint32_t);
void register_leader_election_callback(std::function<void(int)>);
void register_for_leader_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)>,
    uint32_t);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
void add_log_to_nc(const char*, int, uint32_t, int = 0);
void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();
void set_epoch(int epoch = -1);
void upgrade_p1_to_leader();
void worker_info_stats(size_t);
void nc_setup_server(int, std::string);
std::vector<std::vector<int>>* nc_get_new_order_requests(int);
std::vector<std::vector<int>>* nc_get_payment_requests(int);
std::vector<std::vector<int>>* nc_get_delivery_requests(int);
std::vector<std::vector<int>>* nc_get_order_status_requests(int);
std::vector<std::vector<int>>* nc_get_stock_level_requests(int);
std::vector<std::vector<int>>* nc_get_read_requests(int);
std::vector<std::vector<int>>* nc_get_rmw_requests(int);

// Raft-specific function (no-op for Paxos)
void set_preferred_leader(int site_id);
