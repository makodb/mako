#ifndef STATS_REGISTRY_H_
#define STATS_REGISTRY_H_

#include "__dep__.h"
#include <rusty/mutex.hpp>

namespace janus {

// Forward declare ValueTimesPair (defined in rcc_rpc.h)
struct ValueTimesPair;

/**
 * StatsRegistry - Global singleton for statistics registration and collection.
 *
 * This decouples stat collection from ServerControlServiceImpl, allowing
 * services to register their stats without needing a pointer to scsi_.
 */
class StatsRegistry {
public:
    // Stat key constants (moved from ServerControlServiceImpl)
    static const std::string STAT_SZ_SCC;
    static const std::string STAT_N_ASK;
    static const std::string STAT_SZ_GRAPH_START;
    static const std::string STAT_SZ_GRAPH_COMMIT;
    static const std::string STAT_SZ_GRAPH_ASK;

    // Get the singleton instance
    static StatsRegistry& instance();

    // Register a stat pointer by name
    void set_stat(const std::string& name, AvgStat* stat);

    // Get a stat by name (creates if doesn't exist)
    AvgStat* get_stat(const std::string& name);

    // Record a statistic value
    void do_statistics(const char* key, int64_t value_delta);

    // removed `set_recorder(Recorder*)` /
    // `get_recorder()` declarations — see stats_registry.cc retirement
    // comment.

    // Get all stats (for heartbeat response)
    std::map<std::string, AvgStat*> get_all_stats();

    // Get all statistics (for heartbeat response)
    std::unordered_map<const char*, ValueTimesPair> get_all_statistics();

private:
    struct Stats;  // Forward declaration
    rusty::Mutex<Stats>* stats_;

    // Private constructor for singleton
    StatsRegistry();
    ~StatsRegistry();

    // Non-copyable, non-movable
    StatsRegistry(const StatsRegistry&) = delete;
    StatsRegistry& operator=(const StatsRegistry&) = delete;
    StatsRegistry(StatsRegistry&&) = delete;
    StatsRegistry& operator=(StatsRegistry&&) = delete;
};

} // namespace janus

#endif // STATS_REGISTRY_H_
