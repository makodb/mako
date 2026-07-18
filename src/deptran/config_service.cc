/**
 * @file config_service.cc
 * @brief Implementation of ConfigServiceImpl RPC handlers.
 */

#include "config_service.h"

namespace janus {

// @safe
ConfigServiceImpl::ConfigServiceImpl(ConfigStore& store)
    : store_(store),
      cached_data_(rusty::None),
      cached_version_(0),
      cache_valid_(false),
      cached_sharding_policy_(rusty::None),
      cached_sharding_version_(0),
      sharding_cache_valid_(false) {
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::ensure_cache_valid() {
    if (cache_valid_.get()) {
        return;
    }

    // Load config from store
    auto config_opt = store_.load();
    if (config_opt.is_none()) {
        // No config available
        cached_version_.set(0);
        auto guard = cached_data_.lock().unwrap();
        *guard = rusty::None;
        cache_valid_.set(true);
        return;
    }

    PersistentConfig config = config_opt.unwrap();
    cached_version_.set(config.version);

    // Serialize config to string
    auto guard = cached_data_.lock().unwrap();
    *guard = rusty::Some(serialize_config(config));
    cache_valid_.set(true);
}

// @unsafe - Uses Marshal I/O
std::string ConfigServiceImpl::serialize_config(const PersistentConfig& config) {
    rrr::Marshal m;
    rrr::Serialize_::serialize(config, m);

    // Extract data from Marshal to string
    std::string result;
    result.resize(m.content_size());
    m.read(reinterpret_cast<std::uint8_t*>(result.data()), m.content_size());
    return result;
}

// @unsafe - RocksDB I/O, network I/O
void ConfigServiceImpl::GetConfig(
    const ConfigServiceService::RpcGetConfigRequest& rpc_req,
    ConfigServiceService::RpcGetConfigResponse& resp,
    rrr::DeferredReply defer) {
    ensure_cache_valid();

    resp.current_version = cached_version_.get();

    // Check if client already has the latest version
    if (rpc_req.client_version != 0 &&
        rpc_req.client_version == resp.current_version) {
        resp.has_update = 0;
        // config_data left empty (default constructed)
        defer.reply();
        return;
    }

    // Client needs update
    resp.has_update = 1;

    // Copy cached data to response
    auto guard = cached_data_.lock().unwrap();
    if (guard->is_some()) {
        resp.config_data = guard->as_ref().unwrap();
    }

    defer.reply();
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::GetConfigVersion(
    const ConfigServiceService::RpcGetConfigVersionRequest& rpc_req,
    ConfigServiceService::RpcGetConfigVersionResponse& resp,
    rrr::DeferredReply defer) {
    (void)rpc_req;
    // Use store's direct version lookup for efficiency
    resp.version = store_.get_version();
    defer.reply();
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::HasConfig(
    const ConfigServiceService::RpcHasConfigRequest& rpc_req,
    ConfigServiceService::RpcHasConfigResponse& resp,
    rrr::DeferredReply defer) {
    (void)rpc_req;
    resp.has_config = store_.has_config() ? 1 : 0;
    defer.reply();
}

// @safe
void ConfigServiceImpl::invalidate_cache() {
    cache_valid_.set(false);
}

// @safe
void ConfigServiceImpl::invalidate_sharding_cache() {
    sharding_cache_valid_.set(false);
}

// ============================================================================
// Sharding Policy Implementation
// ============================================================================

// @unsafe - RocksDB I/O
void ConfigServiceImpl::ensure_sharding_cache_valid() {
    if (sharding_cache_valid_.get()) {
        return;
    }

    // Load policy from store
    auto policy_opt = store_.load_sharding_policy();
    if (policy_opt.is_none()) {
        // No policy available
        cached_sharding_version_.set(0);
        auto guard = cached_sharding_policy_.lock().unwrap();
        *guard = rusty::None;
        sharding_cache_valid_.set(true);
        return;
    }

    ShardingPolicySet policy = policy_opt.unwrap();
    cached_sharding_version_.set(policy.version);

    // Serialize policy to string
    auto guard = cached_sharding_policy_.lock().unwrap();
    *guard = rusty::Some(serialize_sharding_policy(policy));
    sharding_cache_valid_.set(true);
}

// @unsafe - Uses Marshal I/O
std::string ConfigServiceImpl::serialize_sharding_policy(const ShardingPolicySet& policy) {
    rrr::Marshal m;
    rrr::Serialize_::serialize(policy, m);

    // Extract data from Marshal to string
    std::string result;
    result.resize(m.content_size());
    m.read(reinterpret_cast<std::uint8_t*>(result.data()), m.content_size());
    return result;
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::SetShardingPolicy(
    const ConfigServiceService::RpcSetShardingPolicyRequest& rpc_req,
    ConfigServiceService::RpcSetShardingPolicyResponse& resp,
    rrr::DeferredReply defer) {
    // Deserialize the policy
    rrr::Marshal m;
    m.write_bytes(reinterpret_cast<const std::uint8_t*>(rpc_req.policy_data.data()), rpc_req.policy_data.size());

    ShardingPolicySet policy;
    rrr::Deserialize_::deserialize(policy, m);

    // Save to store
    if (store_.save_sharding_policy(policy)) {
        // Invalidate cache to force re-read on next request
        invalidate_sharding_cache();
        resp.success = 1;
    } else {
        resp.success = 0;
    }

    defer.reply();
}

// @unsafe - RocksDB I/O, network I/O
void ConfigServiceImpl::GetShardingPolicy(
    const ConfigServiceService::RpcGetShardingPolicyRequest& rpc_req,
    ConfigServiceService::RpcGetShardingPolicyResponse& resp,
    rrr::DeferredReply defer) {
    ensure_sharding_cache_valid();

    resp.current_version = cached_sharding_version_.get();

    // Check if client already has the latest version
    if (rpc_req.client_version != 0 &&
        rpc_req.client_version == resp.current_version) {
        resp.has_update = 0;
        // policy_data left empty (default constructed)
        defer.reply();
        return;
    }

    // Client needs update
    resp.has_update = 1;

    // Copy cached data to response
    auto guard = cached_sharding_policy_.lock().unwrap();
    if (guard->is_some()) {
        resp.policy_data = guard->as_ref().unwrap();
    }

    defer.reply();
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::GetShardingPolicyVersion(
    const ConfigServiceService::RpcGetShardingPolicyVersionRequest& rpc_req,
    ConfigServiceService::RpcGetShardingPolicyVersionResponse& resp,
    rrr::DeferredReply defer) {
    (void)rpc_req;
    // Use store's direct version lookup for efficiency
    resp.version = store_.get_sharding_policy_version();
    defer.reply();
}

// @unsafe - RocksDB I/O
void ConfigServiceImpl::HasShardingPolicy(
    const ConfigServiceService::RpcHasShardingPolicyRequest& rpc_req,
    ConfigServiceService::RpcHasShardingPolicyResponse& resp,
    rrr::DeferredReply defer) {
    (void)rpc_req;
    resp.has_policy = store_.has_sharding_policy() ? 1 : 0;
    defer.reply();
}

}  // namespace janus
