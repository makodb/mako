#include <stdint.h>

#include "config_client.h"

import std;

namespace janus {

// @safe
ConfigClient::ConfigClient(const std::string& c_node_addr)
    : c_node_addr_(c_node_addr),
      poll_thread_(rusty::None),
      rpc_client_(rusty::None),
      proxy_(rusty::None) {
}

// @unsafe - calls disconnect which does network I/O
ConfigClient::~ConfigClient() {
    disconnect();
}

// @unsafe - Creates poll thread and establishes network connection
bool ConfigClient::try_connect() {
    // Create poll thread if not already created
    if (poll_thread_.is_none()) {
        poll_thread_ = rusty::Some(rrr::PollThread::create());
    }

    // Create client if not already created
    if (rpc_client_.is_none()) {
        auto& poll = poll_thread_.as_ref().unwrap();
        rpc_client_ = rusty::Some(rrr::Client::create(poll.clone()));
    }

    // Try to connect
    auto& client = rpc_client_.as_ref().unwrap();
    // @unsafe { network connect syscall }
    int result = client->connect(reinterpret_cast<const int8_t*>(c_node_addr_.c_str()), true);

    if (result == 0) {
        // Create proxy
        if (proxy_.is_none()) {
            // @unsafe { new and const_cast for proxy }
            auto proxy = new ConfigServiceProxy(const_cast<rrr::Client*>(client.get()));
            proxy_ = rusty::Some(proxy);
        }
        // @unsafe { logging I/O }
        Log_info("ConfigClient: Connected to c-node at %s", c_node_addr_.c_str());
        return true;
    }

    // @unsafe { logging I/O }
    Log_warn("ConfigClient: Failed to connect to c-node at %s (error: %d)",
             c_node_addr_.c_str(), result);
    return false;
}

// @unsafe - Establishes network connection with retry and sleep
bool ConfigClient::connect() {
    uint32_t retries = 0;
    uint32_t delay_ms = retry_delay_ms_.get();
    uint32_t max_retries = max_retries_.get();
    uint32_t max_delay = max_retry_delay_ms_.get();

    while (retries < max_retries) {
        if (try_connect()) {
            return true;
        }

        retries++;
        if (retries < max_retries) {
            // @unsafe { logging I/O }
            Log_info("ConfigClient: Retrying connection (%u/%u) in %u ms...",
                     retries, max_retries, delay_ms);
            // @unsafe { thread sleep }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

            // Exponential backoff with cap
            delay_ms = std::min(delay_ms * 2, max_delay);
        }
    }

    // @unsafe { logging I/O }
    Log_error("ConfigClient: Failed to connect after %u retries", max_retries);
    return false;
}

// @unsafe - Closes network connection and deletes proxy
void ConfigClient::disconnect() {
    // Delete proxy first (it holds raw pointer to client)
    if (proxy_.is_some()) {
        // @unsafe { delete operator }
        delete proxy_.take().unwrap();
    }

    // Close client connection
    if (rpc_client_.is_some()) {
        // @unsafe { network close }
        rpc_client_.as_ref().unwrap()->close();
        rpc_client_ = rusty::None;
    }

    // Note: We keep poll_thread_ alive - it can be reused for reconnection
}

// @safe
bool ConfigClient::is_connected() const {
    if (rpc_client_.is_none()) {
        return false;
    }
    // Check if client is in connected state
    auto& client = rpc_client_.as_ref().unwrap();
    return client->connection_state() == rrr::ConnectionState::CONNECTED;
}

// @unsafe - Makes RPC call over network
rusty::Option<PersistentConfig> ConfigClient::fetch_config() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot fetch config");
        return rusty::None;
    }

    uint64_t current_version = 0;
    rrr::i32 has_update = 0;
    std::string config_data;

    // Call RPC with client_version=0 to get full config
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcGetConfigRequest req;
    req.client_version = 0;
    // @unsafe { RPC network call }
    auto result = proxy->GetConfig(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: GetConfig RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    auto response = result.unwrap();
    current_version = response.current_version;
    has_update = response.has_update;
    config_data = std::move(response.config_data);

    if (has_update == 0 || config_data.empty()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: C-node has no configuration");
        return rusty::None;
    }

    // Deserialize config_data to PersistentConfig
    rrr::Marshal m;
    // @unsafe { Marshal write not borrow-checked }
    m.write_bytes(reinterpret_cast<const std::uint8_t*>(config_data.data()), config_data.size());

    PersistentConfig config;
    // @unsafe { Marshal read not borrow-checked }
    m >> config;

    // @unsafe { logging I/O }
    Log_info("ConfigClient: Fetched configuration version %lu with %zu sites",
             config.version, config.sites.size());

    return rusty::Some(std::move(config));
}

// @unsafe - Makes RPC call over network
rusty::Option<uint64_t> ConfigClient::fetch_version() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot fetch version");
        return rusty::None;
    }

    uint64_t version = 0;
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcGetConfigVersionRequest req;
    // @unsafe { RPC network call }
    auto result = proxy->GetConfigVersion(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: GetConfigVersion RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    version = result.unwrap().version;

    return rusty::Some(version);
}

// @unsafe - Makes RPC call over network
rusty::Option<bool> ConfigClient::has_config() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot check has_config");
        return rusty::None;
    }

    rrr::i32 has_config_result = 0;
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcHasConfigRequest req;
    // @unsafe { RPC network call }
    auto result = proxy->HasConfig(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: HasConfig RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    has_config_result = result.unwrap().has_config;

    return rusty::Some(has_config_result != 0);
}

// ============================================================================
// Sharding Policy Methods
// ============================================================================

// @unsafe - Makes RPC call over network
rusty::Option<ShardingPolicySet> ConfigClient::fetch_sharding_policy() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot fetch sharding policy");
        return rusty::None;
    }

    uint64_t current_version = 0;
    rrr::i32 has_update = 0;
    std::string policy_data;

    // Call RPC with client_version=0 to get full policy
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcGetShardingPolicyRequest req;
    req.client_version = 0;
    // @unsafe { RPC network call }
    auto result = proxy->GetShardingPolicy(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: GetShardingPolicy RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    auto response = result.unwrap();
    current_version = response.current_version;
    has_update = response.has_update;
    policy_data = std::move(response.policy_data);

    if (has_update == 0 || policy_data.empty()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: C-node has no sharding policy");
        return rusty::None;
    }

    // Deserialize policy_data to ShardingPolicySet
    rrr::Marshal m;
    // @unsafe { Marshal write not borrow-checked }
    m.write_bytes(reinterpret_cast<const std::uint8_t*>(policy_data.data()), policy_data.size());

    ShardingPolicySet policy;
    // @unsafe { Marshal read not borrow-checked }
    m >> policy;

    // @unsafe { logging I/O }
    Log_info("ConfigClient: Fetched sharding policy version %lu with %zu tables",
             policy.version, policy.table_count());

    return rusty::Some(std::move(policy));
}

// @unsafe - Makes RPC call over network
rusty::Option<uint64_t> ConfigClient::fetch_sharding_version() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot fetch sharding version");
        return rusty::None;
    }

    uint64_t version = 0;
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcGetShardingPolicyVersionRequest req;
    // @unsafe { RPC network call }
    auto result = proxy->GetShardingPolicyVersion(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: GetShardingPolicyVersion RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    version = result.unwrap().version;

    return rusty::Some(version);
}

// @unsafe - Makes RPC call over network
rusty::Option<bool> ConfigClient::has_sharding_policy() {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot check has_sharding_policy");
        return rusty::None;
    }

    rrr::i32 has_policy_result = 0;
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcHasShardingPolicyRequest req;
    // @unsafe { RPC network call }
    auto result = proxy->HasShardingPolicy(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: HasShardingPolicy RPC failed with error %d", result.unwrap_err());
        return rusty::None;
    }
    has_policy_result = result.unwrap().has_policy;

    return rusty::Some(has_policy_result != 0);
}

// @unsafe - Makes RPC call over network
bool ConfigClient::set_sharding_policy(const ShardingPolicySet& policy) {
    if (proxy_.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: Not connected, cannot set sharding policy");
        return false;
    }

    // Serialize policy to string
    rrr::Marshal m;
    // @unsafe { Marshal write not borrow-checked }
    m << policy;

    std::string policy_data;
    policy_data.resize(m.content_size());
    // @unsafe { Marshal read not borrow-checked }
    m.read(reinterpret_cast<std::uint8_t*>(policy_data.data()), m.content_size());

    rrr::i32 success = 0;
    auto proxy = proxy_.as_ref().unwrap();
    ConfigServiceProxy::RpcSetShardingPolicyRequest req;
    req.policy_data = std::move(policy_data);
    // @unsafe { RPC network call }
    auto result = proxy->SetShardingPolicy(req);

    if (result.is_err()) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: SetShardingPolicy RPC failed with error %d", result.unwrap_err());
        return false;
    }
    success = result.unwrap().success;

    if (success == 0) {
        // @unsafe { logging I/O }
        Log_warn("ConfigClient: SetShardingPolicy failed on server");
        return false;
    }

    // @unsafe { logging I/O }
    Log_info("ConfigClient: Set sharding policy version %lu with %zu tables",
             policy.version, policy.table_count());
    return true;
}

}  // namespace janus
