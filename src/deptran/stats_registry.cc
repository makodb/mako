#include "stats_registry.h"
#include "procedure.h"  // For SimpleCommand
#include "rcc/dep_graph.h"  // Required before rcc_rpc.h
#include "rcc_rpc.h"

namespace janus {

// Stat key constants
const std::string StatsRegistry::STAT_SZ_SCC = "scc";
const std::string StatsRegistry::STAT_N_ASK = "n_ask";
const std::string StatsRegistry::STAT_SZ_GRAPH_START = "graph_start";
const std::string StatsRegistry::STAT_SZ_GRAPH_COMMIT = "graph_commit";
const std::string StatsRegistry::STAT_SZ_GRAPH_ASK = "graph_ask";
const std::string StatsRegistry::STAT_RO6_SZ_VECTOR = "ro6_sz_vector";

// Stats struct definition (hidden in .cc)
struct StatsRegistry::Stats {
    std::unordered_map<const char*, ValueTimesPair> statistics;
    std::map<std::string, AvgStat*> stats;
    // removed `Recorder* recorder = nullptr;`
    // — only writer was `set_recorder(recorder_)` from
    // `ClassicServiceImpl::RegisterStats` which always passed
    // nullptr.  `set_recorder` and `get_recorder` methods removed
    // in this phase too.
};

StatsRegistry& StatsRegistry::instance() {
    static StatsRegistry instance;
    return instance;
}

StatsRegistry::StatsRegistry() : stats_(new rusty::Mutex<Stats>(Stats{})) {}

StatsRegistry::~StatsRegistry() {
    delete stats_;
}

void StatsRegistry::set_stat(const std::string& name, AvgStat* stat) {
    auto guard = stats_->lock().unwrap();
    guard->stats[name] = stat;
}

AvgStat* StatsRegistry::get_stat(const std::string& name) {
    auto guard = stats_->lock().unwrap();
    auto& stat = guard->stats[name];
    if (stat == nullptr) {
        stat = new AvgStat(AvgStat::new_());
    }
    return stat;
}

void StatsRegistry::do_statistics(const char* key, int64_t value_delta) {
    auto guard = stats_->lock().unwrap();
    auto& pair = guard->statistics[key];
    pair.value += value_delta;
    pair.times++;
}

// removed `set_recorder` / `get_recorder`
// methods — `Stats::recorder` field gone; only writer
// (`ClassicServiceImpl::RegisterStats`) always passed nullptr; only
// reader (`benchmark_control_rpc.cc::server_heart_beat_with_data`)
// always took the else-branch.

std::map<std::string, AvgStat*> StatsRegistry::get_all_stats() {
    auto guard = stats_->lock().unwrap();
    return guard->stats;
}

std::unordered_map<const char*, ValueTimesPair> StatsRegistry::get_all_statistics() {
    auto guard = stats_->lock().unwrap();
    return guard->statistics;
}

} // namespace janus
