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
// Raft is the only supported replication protocol.
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::RAFT};

// Whether a caller named the protocol through set_replication_type*().
// @safe - Using rusty::Cell for interior mutability of a Copy type; written
// during initialization only, like g_replication_type.
// A config file's mode.ab does not set it (apply_config_replication_type),
// which is what lets an explicit choice win over the config files.
static rusty::Cell<bool> g_replication_type_explicit{false};

// @safe - Read-only access through Cell::get()
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @safe - Read-only access through Cell::get()
bool replication_type_is_explicit() {
    return g_replication_type_explicit.get();
}

// @unsafe - Mutation through Cell::set(), plus std::cerr output
static void store_replication_type(ReplicationType type) {
    g_replication_type.set(type);
    // @unsafe { std::cerr output is not borrow-checked }
    std::cerr << "Replication type set to: " << replication_type_to_string(type) << std::endl;
}

// @unsafe - Mutation through Cell::set(), plus std::cerr output
void set_replication_type(ReplicationType type) {
    g_replication_type_explicit.set(true);
    store_replication_type(type);  // @unsafe
}

// @unsafe - Mutation through Cell::set(), plus std::cerr output
bool apply_config_replication_type(ReplicationType type) {
    if (g_replication_type_explicit.get() && g_replication_type.get() != type) {
        // The explicit choice wins; the caller reports the disagreement.
        return false;
    }
    store_replication_type(type);  // @unsafe
    return true;
}

// @safe - String parsing with validation, delegates to set_replication_type
// @unsafe { std::string comparison operators are not borrow-checked }
void set_replication_type_from_string(const std::string& type_str) {
    if (type_str == "raft" || type_str == "RAFT") {
        set_replication_type(ReplicationType::RAFT);
    } else {
        throw std::runtime_error("Invalid replication type: " + type_str +
                                 ". Only 'raft' is supported");
    }
}

// @safe - Pure function, no side effects
const char* replication_type_to_string(ReplicationType type) {
    switch (type) {
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
      throw std::invalid_argument("Unsupported replication protocol: " + std::string(value));
    }
    if (arg == "--replication") {
      if (i + 1 == argc) throw std::invalid_argument("--replication requires raft");
      std::string_view value(argv[++i] ? argv[i] : "");
      if (equals_ignore_case(value, "raft")) {
        return ReplicationType::RAFT;
      }
      throw std::invalid_argument("Unsupported replication protocol: " + std::string(value));
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
      // Config::LoadModeYML validates the same protocol selection.
      if (equals_ignore_case(ab, "raft")) {
        inferred = ReplicationType::RAFT;
      } else if (!equals_ignore_case(ab, "none")) {
        throw std::invalid_argument("Unsupported replication mode.ab: " + ab);
      }
    } catch (const YAML::Exception&) {
      // Ignore unreadable/non-YAML files and keep scanning remaining configs.
    }
  }
  return inferred;
}

// @unsafe - Reads argv and the config files, mutates the global type, and
// writes std::cerr.
// Resolves the protocol setup() dispatches on. Precedence:
//   1. a --replication argument in this argv (an explicit choice);
//   2. an explicit choice made earlier with set_replication_type*(), for
//      example by dbtest's --replication flag, which is not in this argv;
//   3. mode.ab of the -f/--config files;
//   4. the type as it stands: the default, unless a caller already applied
//      a config value.
void resolve_replication_type_from_args(int argc, char* argv[]) {
  if (auto flag_type = parse_replication_flag(argc, argv); flag_type.has_value()) {
    janus::set_replication_type(*flag_type);  // @unsafe
    return;
  }
  const auto config_paths = collect_config_paths(argc, argv);
  if (config_paths.empty()) {
    return;
  }
  const auto config_type = infer_replication_type_from_configs(config_paths);
  if (!config_type.has_value()) {
    return;
  }
  if (!janus::apply_config_replication_type(*config_type)) {  // @unsafe
    // Each worker builds its frame from mode.ab (Frame::GetFrame), so a
    // helper and a mode file that name different protocols cannot run
    // together. Say so here; the explicit choice is kept.
    // @unsafe { std::cerr output is not borrow-checked }
    std::cerr << "Replication type "
              << janus::replication_type_to_string(janus::get_replication_type())
              << " was chosen explicitly but the config files say mode.ab is "
              << janus::replication_type_to_string(*config_type)
              << "; keeping the explicit choice. Pass the mode file of the"
                 " chosen protocol (config/raft.yml)."
              << std::endl;
  }
}

}  // namespace

// Forward the application API to its sole Raft implementation.

std::vector<std::string> setup(int argc, char* argv[]) {
    resolve_replication_type_from_args(argc, argv);  // @unsafe
    return raft_impl::setup(argc, argv);  // @unsafe
}

int setup2(int action, int shardIndex) {
    return raft_impl::setup2(action, shardIndex);  // @unsafe
}

std::map<std::string, std::string> getHosts(std::string s) {
    return raft_impl::getHosts(s);  // @unsafe
}

int get_outstanding_logs(uint32_t par_id) {
    return raft_impl::get_outstanding_logs(par_id);  // @unsafe
}

int shutdown_paxos() {
    return raft_impl::shutdown_paxos();  // @unsafe
}

// removed `microbench_paxos()` dispatcher
// — no callers anywhere; both impls (paxos + raft) deleted.

void register_for_follower(std::function<void(const char*, int)> cb, uint32_t par_id) {
    raft_impl::register_for_follower(cb, par_id);  // @unsafe
}

void register_for_follower_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    raft_impl::register_for_follower_par_id(cb, par_id);  // @unsafe
}

void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
    raft_impl::register_for_follower_par_id_return(cb, par_id);  // @unsafe
}

void register_for_leader(std::function<void(const char*, int)> cb, uint32_t par_id) {
    raft_impl::register_for_leader(cb, par_id);  // @unsafe
}

void register_leader_election_callback(std::function<void(int)> cb) {
    raft_impl::register_leader_election_callback(cb);  // @unsafe
}

void register_for_leader_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    raft_impl::register_for_leader_par_id(cb, par_id);  // @unsafe
}

void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
    raft_impl::register_for_leader_par_id_return(cb, par_id);  // @unsafe
}

void register_snapshot_callbacks_for_partition(uint32_t par_id,
                                               janus::RaftCreateSnapshotFn create,
                                               janus::RaftPrepareSnapshotFn prepare) {
    raft_impl::register_snapshot_callbacks_for_partition(par_id, create, prepare);  // @unsafe
}

void submit(const char* data, int len, uint32_t par_id) {
    raft_impl::submit(data, len, par_id);  // @unsafe
}

void add_log(const char* data, int len, uint32_t par_id) {
    raft_impl::add_log(data, len, par_id);  // @unsafe
}

void add_log_without_queue(const char* data, int len, uint32_t par_id) {
    raft_impl::add_log_without_queue(data, len, par_id);  // @unsafe
}

// @unsafe - forwards borrowed log bytes to Raft admission.
bool add_log_to_nc(const char* data, int len, uint32_t par_id, int flag,
                   uint16_t* leader_hint_out) {
    return raft_impl::add_log_to_nc(data, len, par_id, flag, leader_hint_out);
}

void wait_for_submit(uint32_t par_id) {
    raft_impl::wait_for_submit(par_id);  // @unsafe
}

// removed `microbench_paxos_queue()`
// dispatcher — no callers anywhere; both impls deleted.

void pre_shutdown_step() {
    raft_impl::pre_shutdown_step();  // @unsafe
}

int get_epoch() {
    return raft_impl::get_epoch();  // @unsafe
}

void set_epoch(int epoch) {
    raft_impl::set_epoch(epoch);  // @unsafe
}

void upgrade_p1_to_leader() {
    raft_impl::upgrade_p1_to_leader();  // @unsafe
}

void worker_info_stats(size_t s) {
    raft_impl::worker_info_stats(s);  // @unsafe
}

// Preferred initial leader for Raft.
void set_preferred_leader(int site_id) {
    if (janus::is_using_raft()) {
        raft_impl::set_preferred_leader(site_id);  // @unsafe
    }
}
