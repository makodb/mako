#include "stats_registry.h"
#include "procedure.h"  // For SimpleCommand
#include "rcc_rpc.h"

namespace janus {

// Stats struct definition (hidden in .cc)
struct StatsRegistry::Stats {
    std::unordered_map<const char*, ValueTimesPair> statistics;
    // removed `Recorder* recorder = nullptr;`
    // — the only writer always passed nullptr. `set_recorder` and
    // `get_recorder` were removed with it.
};

StatsRegistry& StatsRegistry::instance() {
    static StatsRegistry instance;
    return instance;
}

StatsRegistry::StatsRegistry() : stats_(new rusty::Mutex<Stats>(Stats{})) {}

StatsRegistry::~StatsRegistry() {
    delete stats_;
}

void StatsRegistry::do_statistics(const char* key, int64_t value_delta) {
    auto guard = stats_->lock().unwrap();
    auto& pair = guard->statistics[key];
    pair.value += value_delta;
    pair.times++;
}

// removed `set_recorder` / `get_recorder`
// methods — `Stats::recorder` field gone; only writer
// the writer always passed nullptr; only
// reader (`benchmark_control_rpc.cc::server_heart_beat_with_data`)
// always took the else-branch.

std::unordered_map<const char*, ValueTimesPair> StatsRegistry::get_all_statistics() {
    auto guard = stats_->lock().unwrap();
    return guard->statistics;
}

} // namespace janus
