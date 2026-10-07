#include <stdint.h>
#include <stddef.h>

#include "replication_helper.h"
#include <yaml-cpp/yaml.h>
#include <rusty/cell.hpp>

import std;

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

// @unsafe - Mutation through Cell::set(), plus std::cerr output
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

namespace {

using janus::ReplicationType;

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<ReplicationType> parse_replication_flag(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i] ? argv[i] : "");
    constexpr std::string_view kPrefix = "--replication=";
    if (arg.rfind(kPrefix, 0) == 0) {
      std::string_view value = arg.substr(kPrefix.size());
      if (equals_ignore_case(value, "raft")) {
        return ReplicationType::RAFT;
      }
      if (equals_ignore_case(value, "paxos")) {
        return ReplicationType::PAXOS;
      }
    }
    if (arg == "--replication" && i + 1 < argc) {
      std::string_view value(argv[++i] ? argv[i] : "");
      if (equals_ignore_case(value, "raft")) {
        return ReplicationType::RAFT;
      }
      if (equals_ignore_case(value, "paxos")) {
        return ReplicationType::PAXOS;
      }
    }
  }
  return std::nullopt;
}

std::vector<std::string> collect_config_paths(int argc, char* argv[]) {
  std::vector<std::string> config_paths;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i] ? argv[i] : "");
    if ((arg == "-f" || arg == "--config") && i + 1 < argc) {
      config_paths.emplace_back(argv[++i]);
      continue;
    }
    constexpr std::string_view kConfigPrefix = "--config=";
    if (arg.rfind(kConfigPrefix, 0) == 0) {
      config_paths.emplace_back(arg.substr(kConfigPrefix.size()));
    }
  }
  return config_paths;
}

std::optional<ReplicationType> infer_replication_type_from_configs(
    const std::vector<std::string>& config_paths) {
  std::optional<ReplicationType> inferred;
  for (const auto& path : config_paths) {
    try {
      YAML::Node config = YAML::LoadFile(path);
      if (!config["mode"] || !config["mode"]["ab"]) {
        continue;
      }
      const std::string ab = config["mode"]["ab"].as<std::string>();
      if (equals_ignore_case(ab, "raft")) {
        inferred = ReplicationType::RAFT;
      } else if (equals_ignore_case(ab, "paxos")) {
        inferred = ReplicationType::PAXOS;
      }
    } catch (const std::exception&) {
      // Ignore unreadable/non-YAML files and keep scanning remaining configs.
    }
  }
  return inferred;
}

std::optional<ReplicationType> infer_replication_type_from_args(int argc, char* argv[]) {
  if (auto flag_type = parse_replication_flag(argc, argv); flag_type.has_value()) {
    return flag_type;
  }
  const auto config_paths = collect_config_paths(argc, argv);
  if (config_paths.empty()) {
    return std::nullopt;
  }
  return infer_replication_type_from_configs(config_paths);
}

}  // namespace

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
    if (auto inferred = infer_replication_type_from_args(argc, argv); inferred.has_value()) {
        janus::set_replication_type(*inferred);
    }
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

bool is_replication_leader(uint32_t par_id) {
    DISPATCH_RAFT_OR_PAXOS(is_replication_leader, par_id);  // @unsafe
}

int shutdown_paxos() {
    DISPATCH_RAFT_OR_PAXOS(shutdown_paxos);  // @unsafe
}

// removed `microbench_paxos()` dispatcher
// — no callers anywhere; both impls (paxos + raft) deleted.

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

// @unsafe - dispatches to Raft or Paxos implementation
bool add_log_to_nc(const char* data, int len, uint32_t par_id, int flag,
                   uint16_t* leader_hint_out) {
    auto type = janus::get_replication_type();
    if (type == janus::ReplicationType::RAFT) {
        return raft_impl::add_log_to_nc(data, len, par_id, flag, leader_hint_out);  // @unsafe
    } else {
        paxos_impl::add_log_to_nc(data, len, par_id, flag);  // @unsafe
        return true;  // Paxos always accepts (leader-only operation)
    }
}

void wait_for_submit(uint32_t par_id) {
    DISPATCH_VOID_RAFT_OR_PAXOS(wait_for_submit, par_id);  // @unsafe
}

// removed `microbench_paxos_queue()`
// dispatcher — no callers anywhere; both impls deleted.

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

// Raft-specific function - no-op for Paxos
void set_preferred_leader(int site_id) {
    if (janus::is_using_raft()) {
        raft_impl::set_preferred_leader(site_id);  // @unsafe
    }
    // No-op for Paxos
}
