#pragma once
#include "config_manager.h"
#include "cluster_config.h"
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

namespace janus {

/**
 * ConfigWatcher - Background polling mechanism that watches for config version
 * changes in a ConfigManager/ReplicatedDB and updates a local ClusterConfig
 * cache when changes are detected.
 *
 * In production, ConfigWatcher runs on non-shard-0 nodes and polls shard 0's
 * leader via RPC. This implementation provides the core polling logic that
 * works within a single process (watches a ReplicatedDB directly). The
 * cross-shard RPC layer can be added later.
 *
 * Usage:
 *   ConfigManager cm(&rdb);
 *   ClusterConfig local;
 *   ConfigWatcher watcher(&cm, &local, 500);  // poll every 500ms
 *   watcher.SetUpdateCallback([](const ClusterConfig& c) { ... });
 *   watcher.Start();   // background thread
 *   // ... later ...
 *   watcher.Stop();
 *
 * Or manually:
 *   watcher.Poll();  // single check, returns true if config was updated
 */
// @unsafe - Manages background thread, stores non-owning raw pointers
class ConfigWatcher {
public:
    using UpdateCallback = std::function<void(const ClusterConfig&)>;

    // @unsafe - Stores non-owning raw pointers to ConfigManager and ClusterConfig
    ConfigWatcher(ConfigManager* cm, ClusterConfig* local_config,
                  uint64_t poll_interval_ms = 1000);

    // @unsafe - Calls Stop() which joins thread
    ~ConfigWatcher();

    // Non-copyable, non-movable
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    // Manual poll - check for updates and apply if version changed.
    // Returns true if config was updated, false if no change or error.
    // @unsafe - Reads from ConfigManager (RocksDB/Raft), updates ClusterConfig
    bool Poll();

    // Start background polling thread.
    // @unsafe - Creates std::thread
    void Start();

    // Stop background polling thread (blocks until thread exits).
    // @unsafe - Joins std::thread
    void Stop();

    // Set callback invoked on config update (called from polling thread or Poll caller).
    // @unsafe - Stores std::function
    void SetUpdateCallback(UpdateCallback cb);

    // @safe - Atomic read
    bool IsRunning() const { return running_.load(); }

    // @safe - Read-only (only modified from Poll which is single-threaded per watcher)
    uint64_t GetLastVersion() const { return last_version_; }

    // @safe - Read-only
    uint64_t GetPollCount() const { return poll_count_; }

private:
    ConfigManager* cm_;            // Non-owning, lifetime managed externally
    ClusterConfig* local_config_;  // Non-owning, lifetime managed externally
    uint64_t poll_interval_ms_;
    uint64_t last_version_ = 0;
    uint64_t poll_count_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread poll_thread_;
    UpdateCallback update_callback_;
};

} // namespace janus
