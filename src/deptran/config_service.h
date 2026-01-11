#pragma once

/**
 * @file config_service.h
 * @brief RPC service implementation for configuration node.
 *
 * The ConfigServiceImpl serves cluster configuration to other nodes.
 * It wraps ConfigStore for persistence and provides caching for efficiency.
 */

#include "__dep__.h"
#include "procedure.h"
#include "rcc/tx.h"
#include "rcc_rpc.h"
#include "config_store.h"
#include "config_schema.h"
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/mutex.hpp>

namespace janus {

/**
 * @brief RPC service implementation for serving configuration.
 *
 * This service runs on the configuration node (c-node) and serves
 * cluster configuration to other nodes via RPC.
 *
 * Thread-safety: Uses rusty::Mutex for cache synchronization.
 */
// @safe - Service implementation using rusty types for thread safety
class ConfigServiceImpl : public ConfigServiceService {
private:
    // Reference to the underlying ConfigStore (owned externally)
    ConfigStore& store_;

    // Cache for serialized configuration (avoid re-serialization on each request)
    // Protected by mutex for thread-safe access
    rusty::Mutex<rusty::Option<std::string>> cached_data_;

    // Cached version number (quick check without loading full config)
    rusty::Cell<uint64_t> cached_version_;

    // Whether cache is valid
    rusty::Cell<bool> cache_valid_;

    // Helper: Load and cache configuration
    // @unsafe - RocksDB I/O
    void ensure_cache_valid();

    // Helper: Serialize config to string
    // @unsafe - Uses Marshal I/O
    std::string serialize_config(const PersistentConfig& config);

public:
    // Constructor takes reference to ConfigStore (managed externally)
    // @safe
    explicit ConfigServiceImpl(ConfigStore& store);

    // Destructor
    // @safe
    ~ConfigServiceImpl() = default;

    // Non-copyable, non-movable (owns mutex)
    ConfigServiceImpl(const ConfigServiceImpl&) = delete;
    ConfigServiceImpl& operator=(const ConfigServiceImpl&) = delete;
    ConfigServiceImpl(ConfigServiceImpl&&) = delete;
    ConfigServiceImpl& operator=(ConfigServiceImpl&&) = delete;

    // =========================================================================
    // RPC Handlers
    // =========================================================================

    /**
     * @brief Get full configuration with version checking.
     *
     * If client_version matches current version, returns has_update=0
     * to avoid unnecessary data transfer.
     *
     * @param client_version Client's known config version (0 = always fetch)
     * @param current_version [out] Current config version on server
     * @param has_update [out] 1 if config has changed since client_version, 0 otherwise
     * @param config_data [out] Serialized PersistentConfig (empty if no update)
     */
    // @unsafe - RocksDB I/O, network I/O
    void GetConfig(const uint64_t& client_version,
                   uint64_t* current_version,
                   rrr::i32* has_update,
                   std::string* config_data,
                   rrr::DeferredReply defer) override;

    /**
     * @brief Get just the config version (lightweight check).
     *
     * Clients can poll this to detect config changes before fetching full config.
     *
     * @param version [out] Current config version on server
     */
    // @unsafe - RocksDB I/O
    void GetConfigVersion(uint64_t* version,
                          rrr::DeferredReply defer) override;

    /**
     * @brief Check if configuration is available.
     *
     * @param result [out] 1 if config exists in store, 0 otherwise
     */
    // @unsafe - RocksDB I/O
    void HasConfig(rrr::i32* result,
                   rrr::DeferredReply defer) override;

    // =========================================================================
    // Cache Management
    // =========================================================================

    /**
     * @brief Invalidate the cache.
     *
     * Call this after updating configuration to force re-serialization
     * on next request.
     */
    // @safe
    void invalidate_cache();
};

}  // namespace janus
