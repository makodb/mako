#include "config_watcher.h"
#include "../__dep__.h"

using namespace janus;

// @unsafe - Stores non-owning raw pointers to ConfigManager and ClusterConfig
ConfigWatcher::ConfigWatcher(ConfigManager* cm, ClusterConfig* local_config,
                             uint64_t poll_interval_ms)
    : cm_(cm), local_config_(local_config), poll_interval_ms_(poll_interval_ms) {
}

// @unsafe - Calls Stop() which joins thread
ConfigWatcher::~ConfigWatcher() {
    Stop();
}

// @unsafe - Reads from ConfigManager (RocksDB/Raft), updates ClusterConfig, invokes callback
bool ConfigWatcher::Poll() {
    poll_count_++;

    // Read current version from ConfigManager
    // @unsafe { ConfigManager::GetVersion reads from RocksDB }
    uint64_t current_version = 0;
    try {
        current_version = cm_->GetVersion();
    } catch (...) {
        // @unsafe { Log_warn is not borrow-checked }
        Log_warn("[CONFIG-WATCHER] Failed to read version from ConfigManager");
        return false;
    }

    if (current_version == last_version_) {
        return false;  // No change
    }

    // Version changed - reload full config from ConfigManager
    // @unsafe { LoadFromConfigManager reads from RocksDB via ConfigManager }
    bool ok = false;
    try {
        ok = local_config_->LoadFromConfigManager(cm_);
    } catch (...) {
        // @unsafe { Log_warn is not borrow-checked }
        Log_warn("[CONFIG-WATCHER] Failed to load config from ConfigManager");
        return false;
    }

    if (!ok) {
        // @unsafe { Log_warn is not borrow-checked }
        Log_warn("[CONFIG-WATCHER] LoadFromConfigManager returned false");
        return false;
    }

    last_version_ = current_version;
    // @unsafe { Log_info is not borrow-checked }
    Log_info("[CONFIG-WATCHER] Config updated to version {}", current_version);

    // Invoke callback if set
    if (update_callback_) {
        try {
            update_callback_(*local_config_);
        } catch (...) {
            // @unsafe { Log_warn is not borrow-checked }
            Log_warn("[CONFIG-WATCHER] Update callback threw exception");
        }
    }

    return true;
}

// @unsafe - Creates std::thread
void ConfigWatcher::Start() {
    if (running_.load()) return;  // Already running

    stop_requested_.store(false);
    running_.store(true);

    // @unsafe { std::thread creation }
    poll_thread_ = std::thread([this]() {
        while (!stop_requested_.load()) {
            try {
                Poll();
            } catch (...) {
                // @unsafe { Log_warn is not borrow-checked }
                Log_warn("[CONFIG-WATCHER] Poll threw unexpected exception, retrying...");
            }
            // @unsafe { std::this_thread::sleep_for is not borrow-checked }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(poll_interval_ms_));
        }
        running_.store(false);
    });
}

// @unsafe - Joins std::thread
void ConfigWatcher::Stop() {
    stop_requested_.store(true);
    if (poll_thread_.joinable()) {
        // @unsafe { std::thread::join }
        poll_thread_.join();
    }
}

// @unsafe - Stores std::function
void ConfigWatcher::SetUpdateCallback(UpdateCallback cb) {
    update_callback_ = std::move(cb);
}
