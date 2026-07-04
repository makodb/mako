#pragma once

#include "__dep__.h"
#include "procedure.h"
#include "rcc/tx.h"
#include "rcc_rpc.h"
#include "config_schema.h"
#include "cluster/sharding_policy.h"
#include "cluster/sharding_policy_marshal.h"
#include <rusty/option.hpp>
#include <rusty/cell.hpp>

namespace janus {

/**
 * Configuration client for fetching cluster configuration from a c-node.
 *
 * Usage:
 *   ConfigClient client("192.168.1.1:8080");
 *   if (client.connect()) {
 *       auto config = client.fetch_config();
 *       if (config.is_some()) {
 *           // Use config.unwrap()
 *       }
 *   }
 *
 * RustyCpp Safety:
 * - Uses rusty::Option for nullable returns
 * - Uses rusty::Cell for interior mutability of retry state
 * - @safe annotations where applicable
 */
class ConfigClient {
private:
    std::string c_node_addr_;                      // Address of config node (host:port)
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;  // Polling thread
    rusty::Option<rusty::Arc<rrr::Client>> rpc_client_;       // RPC client
    rusty::Option<ConfigServiceProxy*> proxy_;    // Generated proxy (owned, nullable)

    // Retry configuration - using Cell for interior mutability
    rusty::Cell<uint32_t> max_retries_{10};
    rusty::Cell<uint32_t> retry_delay_ms_{1000};
    rusty::Cell<uint32_t> max_retry_delay_ms_{30000};
    rusty::Cell<uint32_t> connect_timeout_ms_{5000};

    // @unsafe - Creates poll thread and client connection
    bool try_connect();

public:
    // @safe - Simple construction, no I/O
    explicit ConfigClient(const std::string& c_node_addr);

    // @unsafe - Cleanup, releases network resources
    ~ConfigClient();

    // Delete copy operations (has owned resources)
    ConfigClient(const ConfigClient&) = delete;
    ConfigClient& operator=(const ConfigClient&) = delete;

    // Allow move operations
    ConfigClient(ConfigClient&&) = default;
    ConfigClient& operator=(ConfigClient&&) = default;

    /**
     * Connect to the configuration node.
     * Uses exponential backoff retry on failure.
     *
     * @return true if connected, false if all retries exhausted
     */
    // @unsafe - Establishes network connection
    bool connect();

    /**
     * Disconnect from the configuration node.
     * Safe to call multiple times.
     */
    // @unsafe - Closes network connection
    void disconnect();

    /**
     * Check if currently connected to c-node.
     */
    // @safe - Read-only check
    bool is_connected() const;

    /**
     * Fetch full configuration from c-node.
     * Must be connected first.
     *
     * @return Option containing PersistentConfig, or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<PersistentConfig> fetch_config();

    /**
     * Fetch only the configuration version (lightweight check).
     * Must be connected first.
     *
     * @return Option containing version, or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<uint64_t> fetch_version();

    /**
     * Check if c-node has any configuration stored.
     * Must be connected first.
     *
     * @return Option containing bool (true if config exists), or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<bool> has_config();

    // =========================================================================
    // Sharding Policy Methods
    // =========================================================================

    /**
     * Fetch full sharding policy from c-node.
     * Must be connected first.
     *
     * @return Option containing ShardingPolicySet, or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<ShardingPolicySet> fetch_sharding_policy();

    /**
     * Fetch only the sharding policy version (lightweight check).
     * Must be connected first.
     *
     * @return Option containing version, or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<uint64_t> fetch_sharding_version();

    /**
     * Check if c-node has any sharding policy stored.
     * Must be connected first.
     *
     * @return Option containing bool (true if policy exists), or None on failure
     */
    // @unsafe - Makes RPC call
    rusty::Option<bool> has_sharding_policy();

    /**
     * Set sharding policy on c-node.
     * Must be connected first.
     *
     * @param policy The sharding policy to set
     * @return true on success, false on failure
     */
    // @unsafe - Makes RPC call
    bool set_sharding_policy(const ShardingPolicySet& policy);

    // Configuration setters (all @safe)
    void set_max_retries(uint32_t retries) { max_retries_.set(retries); }
    void set_retry_delay_ms(uint32_t delay_ms) { retry_delay_ms_.set(delay_ms); }
    void set_max_retry_delay_ms(uint32_t max_delay_ms) { max_retry_delay_ms_.set(max_delay_ms); }
    void set_connect_timeout_ms(uint32_t timeout_ms) { connect_timeout_ms_.set(timeout_ms); }

    // Configuration getters (all @safe)
    uint32_t max_retries() const { return max_retries_.get(); }
    uint32_t retry_delay_ms() const { return retry_delay_ms_.get(); }
    uint32_t max_retry_delay_ms() const { return max_retry_delay_ms_.get(); }
    uint32_t connect_timeout_ms() const { return connect_timeout_ms_.get(); }
};

}  // namespace janus
