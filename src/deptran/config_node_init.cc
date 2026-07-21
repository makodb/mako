/**
 * @file config_node_init.cc
 * @brief Implementation of config node initialization functions.
 */

#include "config_node_init.h"
#include "config_store.h"
#include "config_service.h"
#include "config_client.h"
#include "sharding_policy_cache.h"
#include "rrr/rrr.hpp"

namespace janus {

// Global config node components
static ConfigStore* g_config_store = nullptr;
static rusty::Option<rusty::Arc<rrr::PollThread>> g_config_poll_thread = rusty::None;
static rrr::Server* g_config_rpc_server = nullptr;

// @unsafe - RocksDB I/O and RPC server startup
bool init_config_node() {
    auto& settings = ConfigNodeSettings::instance();

    if (!settings.is_config_node()) {
        return true;  // Not a c-node, nothing to do
    }

    // @unsafe { logging I/O }
    Log_info("Initializing config node at {}", settings.config_db_path().c_str());

    // Create and open ConfigStore
    // @unsafe { new operator }
    g_config_store = new ConfigStore(settings.config_db_path());
    // @unsafe { RocksDB I/O }
    if (!g_config_store->open()) {
        // @unsafe { logging I/O }
        Log_warn("Failed to open ConfigStore at {}", settings.config_db_path().c_str());
        delete g_config_store;
        g_config_store = nullptr;
        return false;
    }

    // Check if this is first boot or reboot
    // @unsafe { RocksDB I/O }
    bool has_stored_config = g_config_store->has_config();

    if (!has_stored_config) {
        // First boot: would save config from YAML to RocksDB
        // This requires the Config singleton to be already loaded
        Log_info("Config node first boot - no stored config yet");
        // Config saving will be done when Config is loaded later
    } else {
        // Reboot: load from RocksDB
        Log_info("Config node reboot - loading from RocksDB");

        // @unsafe { RocksDB I/O }
        auto stored_config = g_config_store->load();
        if (stored_config.is_some()) {
            PersistentConfig persistent = stored_config.unwrap();
            Log_info("Loaded config version {} from RocksDB", persistent.version);
            // Config application will be done by caller
        } else {
            Log_warn("Failed to load config from RocksDB on reboot");
        }

        // Load sharding policy from RocksDB if exists
        // @unsafe { RocksDB I/O }
        if (g_config_store->has_sharding_policy()) {
            auto policy_opt = g_config_store->load_sharding_policy();
            if (policy_opt.is_some()) {
                ShardingPolicySet policy = policy_opt.unwrap();
                Log_info("Loaded sharding policy version {} with {} tables from RocksDB",
                         policy.version, policy.table_count());
                // Initialize global sharding policy cache
                get_sharding_policy_cache().set_policy(std::move(policy));
            } else {
                Log_warn("Failed to load sharding policy from RocksDB on reboot");
            }
        } else {
            Log_info("No sharding policy stored yet - waiting for initializer");
        }
    }

    // Start RPC server
    int port = settings.config_port();
    std::string bind_addr = "0.0.0.0:" + std::to_string(port);

    // @unsafe { RPC thread creation }
    g_config_poll_thread = rusty::Some(rrr::PollThread::create());
    g_config_rpc_server = new rrr::Server(rrr::Server::new_(rusty::Some(g_config_poll_thread.as_ref().unwrap().clone())));

    // @unsafe { RPC service registration - ConfigServiceImpl ownership transferred to server }
    g_config_rpc_server->reg_service_typed(rusty::make_box<ConfigServiceImpl>(*g_config_store));

    // @unsafe { RPC server start }
    if (g_config_rpc_server->start(reinterpret_cast<const int8_t*>(bind_addr.c_str())) != 0) {
        Log_warn("Failed to start ConfigService RPC server on {}", bind_addr.c_str());
        delete g_config_rpc_server;
        g_config_rpc_server = nullptr;
        g_config_poll_thread = rusty::None;
        return false;
    }

    Log_info("ConfigService RPC server started on {}", bind_addr.c_str());
    return true;
}

// @unsafe - Network I/O
bool fetch_config_from_cnode() {
    auto& settings = ConfigNodeSettings::instance();

    const std::string& c_node_addr = settings.config_node_addr();
    if (c_node_addr.empty()) {
        return false;  // No c-node address specified
    }

    Log_info("Fetching configuration from c-node at {}", c_node_addr.c_str());

    // @unsafe { network client creation }
    ConfigClient client(c_node_addr);
    client.set_max_retries(5);
    client.set_retry_delay_ms(500);

    // @unsafe { network connect }
    if (!client.connect()) {
        Log_warn("Failed to connect to c-node at {}", c_node_addr.c_str());
        return false;
    }

    // @unsafe { RPC call }
    auto config_opt = client.fetch_config();
    if (config_opt.is_none()) {
        Log_warn("Failed to fetch config from c-node");
        client.disconnect();
        return false;
    }

    PersistentConfig persistent = config_opt.unwrap();
    Log_info("Fetched config version {} from c-node", persistent.version);

    // Config application will be done by caller
    // @unsafe { network disconnect }
    client.disconnect();
    return true;
}

// @unsafe - Network I/O
bool fetch_sharding_policy_from_cnode() {
    auto& settings = ConfigNodeSettings::instance();

    const std::string& c_node_addr = settings.config_node_addr();
    if (c_node_addr.empty()) {
        Log_debug("No c-node address specified, skipping sharding policy fetch");
        return false;
    }

    // Check if already initialized
    if (get_sharding_policy_cache().is_initialized()) {
        Log_info("Sharding policy already initialized, skipping c-node fetch");
        return true;
    }

    Log_info("Fetching sharding policy from c-node at {}", c_node_addr.c_str());

    // @unsafe { network client creation }
    ConfigClient client(c_node_addr);
    client.set_max_retries(10);  // More retries for policy - it may not be set yet
    client.set_retry_delay_ms(500);

    // @unsafe { network connect }
    if (!client.connect()) {
        Log_warn("Failed to connect to c-node at {}", c_node_addr.c_str());
        return false;
    }

    // Check if c-node has a sharding policy
    // @unsafe { RPC call }
    auto has_policy_opt = client.has_sharding_policy();
    if (has_policy_opt.is_none()) {
        Log_warn("Failed to check if c-node has sharding policy");
        client.disconnect();
        return false;
    }

    if (!has_policy_opt.unwrap()) {
        Log_info("C-node does not have a sharding policy yet - will use fallback routing");
        client.disconnect();
        return true;  // Not an error, just no policy set yet
    }

    // @unsafe { RPC call }
    auto policy_opt = client.fetch_sharding_policy();
    if (policy_opt.is_none()) {
        Log_warn("Failed to fetch sharding policy from c-node");
        client.disconnect();
        return false;
    }

    ShardingPolicySet policy = policy_opt.unwrap();
    Log_info("Fetched sharding policy version {} with {} tables from c-node",
             policy.version, policy.table_count());

    // Initialize global sharding policy cache
    get_sharding_policy_cache().set_policy(std::move(policy));
    Log_info("Sharding policy cache initialized");

    // @unsafe { network disconnect }
    client.disconnect();
    return true;
}

// @unsafe - Network and RocksDB I/O
void shutdown_config_node() {
    if (g_config_rpc_server != nullptr) {
        Log_info("Shutting down ConfigService RPC server");
        // @unsafe { RPC server graceful shutdown }
        g_config_rpc_server->graceful_shutdown(1000);  // 1 second timeout
        delete g_config_rpc_server;
        g_config_rpc_server = nullptr;
    }

    if (g_config_poll_thread.is_some()) {
        g_config_poll_thread = rusty::None;
    }

    if (g_config_store != nullptr) {
        // @unsafe { RocksDB close }
        g_config_store->close();
        delete g_config_store;
        g_config_store = nullptr;
    }
}

}  // namespace janus
