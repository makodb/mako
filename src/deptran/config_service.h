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
#include "cluster/config_store.h"
#include "config_schema.h"
#include "cluster/sharding_policy.h"
#include "cluster/sharding_policy_marshal.h"
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

    // Cache for serialized sharding policy
    rusty::Mutex<rusty::Option<std::string>> cached_sharding_policy_;

    // Cached sharding policy version
    rusty::Cell<uint64_t> cached_sharding_version_;

    // Whether sharding policy cache is valid
    rusty::Cell<bool> sharding_cache_valid_;

    // Helper: Load and cache configuration
    // @unsafe - RocksDB I/O
    void ensure_cache_valid();

    // Helper: Load and cache sharding policy
    // @unsafe - RocksDB I/O
    void ensure_sharding_cache_valid();

    // Helper: Serialize config to string
    // @unsafe - Uses Marshal I/O
    std::string serialize_config(const PersistentConfig& config);

    // Helper: Serialize sharding policy to string
    // @unsafe - Uses Marshal I/O
    std::string serialize_sharding_policy(const ShardingPolicySet& policy);

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
    // Cache Management
    // =========================================================================

    /**
     * @brief Invalidate the configuration cache.
     *
     * Call this after updating configuration to force re-serialization
     * on next request.
     */
    // @safe
    void invalidate_cache();

    /**
     * @brief Invalidate the sharding policy cache.
     *
     * Call this after updating sharding policy to force re-serialization
     * on next request.
     */
    // @safe
    void invalidate_sharding_cache();

  // BEGIN typed-rpc-decls (ConfigServiceImpl)
  // Typed RPC interface overrides (new API).
  void GetConfig(const ConfigServiceService::RpcGetConfigRequest& req, ConfigServiceService::RpcGetConfigResponse& resp, rrr::DeferredReply defer) override;
  void GetConfigVersion(const ConfigServiceService::RpcGetConfigVersionRequest& req, ConfigServiceService::RpcGetConfigVersionResponse& resp, rrr::DeferredReply defer) override;
  void HasConfig(const ConfigServiceService::RpcHasConfigRequest& req, ConfigServiceService::RpcHasConfigResponse& resp, rrr::DeferredReply defer) override;
  void SetShardingPolicy(const ConfigServiceService::RpcSetShardingPolicyRequest& req, ConfigServiceService::RpcSetShardingPolicyResponse& resp, rrr::DeferredReply defer) override;
  void GetShardingPolicy(const ConfigServiceService::RpcGetShardingPolicyRequest& req, ConfigServiceService::RpcGetShardingPolicyResponse& resp, rrr::DeferredReply defer) override;
  void GetShardingPolicyVersion(const ConfigServiceService::RpcGetShardingPolicyVersionRequest& req, ConfigServiceService::RpcGetShardingPolicyVersionResponse& resp, rrr::DeferredReply defer) override;
  void HasShardingPolicy(const ConfigServiceService::RpcHasShardingPolicyRequest& req, ConfigServiceService::RpcHasShardingPolicyResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (ConfigServiceImpl)
};

}  // namespace janus
