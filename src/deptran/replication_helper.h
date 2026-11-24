#pragma once

// Raft-only application replication interface.
// Explicit protocol selections and mode.ab values must name Raft.

#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <vector>
#include <cstdint>

#include "raft/snapshot_callbacks.h"

namespace janus {

// Replication type enum - Copy type suitable for rusty::Cell
// @safe - trivially copyable enum
enum class ReplicationType : int {
    RAFT = 1
};

// Get/set the current replication type (default: RAFT)
// @safe - uses Cell for interior mutability
ReplicationType get_replication_type();
// An explicit choice: it marks the type as chosen by the caller, so that no
// config file can change it afterwards.
void set_replication_type(ReplicationType type);
void set_replication_type_from_string(const std::string& type_str);
const char* replication_type_to_string(ReplicationType type);

// True once set_replication_type*() has run in this process.
// @safe - read-only accessor
bool replication_type_is_explicit();

// Applies the protocol named by a config file's mode.ab. It does not count as
// an explicit choice. Returns false, and changes nothing, when an explicit
// choice names the other protocol: the explicit choice wins.
// @unsafe - mutation through Cell::set(), plus std::cerr output
bool apply_config_replication_type(ReplicationType type);

// Check if using specific replication type
// @safe - read-only accessors
inline bool is_using_raft() { return get_replication_type() == ReplicationType::RAFT; }

}  // namespace janus

// ============================================================================
// Raft Implementation Namespace Forward Declarations
// ============================================================================
namespace raft_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
std::map<std::string, std::string> getHosts(std::string);
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
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
// The embedder's state-machine snapshot callbacks for one partition's Raft
// server.
void register_snapshot_callbacks_for_partition(uint32_t par_id,
                                               janus::RaftCreateSnapshotFn create,
                                               janus::RaftPrepareSnapshotFn prepare);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
bool add_log_to_nc(const char*, int, uint32_t, int, uint16_t* leader_hint_out = nullptr);
void wait_for_submit(uint32_t);
void pre_shutdown_step();
int get_epoch();
void set_epoch(int epoch);
void upgrade_p1_to_leader();
void worker_info_stats(size_t);
// removed `microbench_paxos()` and
// `microbench_paxos_queue()` declarations (raft_impl side) — both
// stubs and dispatcher deleted.
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
// The embedder's state-machine snapshot callbacks for one partition's Raft
// server.
void register_snapshot_callbacks_for_partition(uint32_t par_id,
                                               janus::RaftCreateSnapshotFn create,
                                               janus::RaftPrepareSnapshotFn prepare);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
bool add_log_to_nc(const char*, int, uint32_t, int = 0, uint16_t* leader_hint_out = nullptr);
void wait_for_submit(uint32_t);
void pre_shutdown_step();
int get_epoch();
void set_epoch(int epoch = -1);
void upgrade_p1_to_leader();
void worker_info_stats(size_t);
// removed `microbench_paxos()` and
// `microbench_paxos_queue()` declarations (unified API side) — both
// dispatcher impls deleted.

// Preferred initial leader for Raft.
void set_preferred_leader(int site_id);
