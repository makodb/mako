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
    // Get the singleton instance
    static StatsRegistry& instance();

    // Record a statistic value
    void do_statistics(const char* key, int64_t value_delta);

    // removed `set_recorder(Recorder*)` /
    // `get_recorder()` declarations — see stats_registry.cc retirement
    // comment.

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
