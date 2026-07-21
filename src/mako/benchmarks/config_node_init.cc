/**
 * @file config_node_init.cc
 * @brief Implementation of config node initialization functions.
 */

#include "config_node_init.h"
#include "deptran/config_converter.h"
#include "deptran/sharding_policy_cache.h"
#include "rrr/rrr.hpp"

namespace mako {

// Global config node components
janus::ConfigStore* g_config_store = nullptr;
rusty::Option<rusty::Arc<rrr::PollThread>> g_config_poll_thread = rusty::None;
rrr::Server* g_config_rpc_server = nullptr;

// @unsafe - RocksDB I/O and RPC server startup
bool init_config_node() {
    auto& benchConfig = BenchmarkConfig::getInstance();

    if (!benchConfig.isConfigNode()) {
        return true;  // Not a c-node, nothing to do
    }

    rrr::Log_info("Initializing config node at {}", benchConfig.getConfigDbPath().c_str());

    // Create and open ConfigStore
    // @unsafe { new operator }
    g_config_store = new janus::ConfigStore(benchConfig.getConfigDbPath());
    // @unsafe { RocksDB I/O }
    if (!g_config_store->open()) {
        // @unsafe { logging I/O }
        rrr::Log_warn("Failed to open ConfigStore at {}", benchConfig.getConfigDbPath().c_str());
        delete g_config_store;
        g_config_store = nullptr;
        return false;
    }

    // Check if this is first boot or reboot
    // @unsafe { RocksDB I/O }
    bool has_stored_config = g_config_store->has_config();

    if (!has_stored_config) {
        // First boot: load from YAML and save to RocksDB
        rrr::Log_info("Config node first boot - loading from YAML");

        transport::Configuration* transport_config = benchConfig.getConfig();
        if (transport_config != nullptr) {
            // Convert transport config to persistent config
            // @safe { pure data transformation }
            janus::PersistentConfig persistent = janus::from_transport_config(
                *transport_config, 1);  // Version 1 for first boot

            // Save to RocksDB
            // @unsafe { RocksDB I/O }
            if (!g_config_store->save(persistent)) {
                rrr::Log_warn("Failed to save initial config to RocksDB");
            } else {
                rrr::Log_info("Saved initial config to RocksDB (version {})", persistent.version);
            }
        } else {
            rrr::Log_warn("No transport configuration available for first boot");
        }
    } else {
        // Reboot: load from RocksDB
        rrr::Log_info("Config node reboot - loading from RocksDB");

        // @unsafe { RocksDB I/O }
        auto stored_config = g_config_store->load();
        if (stored_config.is_some()) {
            janus::PersistentConfig persistent = stored_config.unwrap();
            rrr::Log_info("Loaded config version {} from RocksDB", persistent.version);

            // Convert to transport config if not already set
            if (benchConfig.getConfig() == nullptr) {
                // @unsafe { memory allocation }
                auto transport_opt = janus::to_transport_config(persistent);
                if (transport_opt.is_some()) {
                    benchConfig.setConfig(transport_opt.unwrap());
                    rrr::Log_info("Restored transport configuration from RocksDB");
                }
            }
        } else {
            rrr::Log_warn("Failed to load config from RocksDB on reboot");
        }

        // Load sharding policy from RocksDB if exists
        // @unsafe { RocksDB I/O }
        if (g_config_store->has_sharding_policy()) {
            auto policy_opt = g_config_store->load_sharding_policy();
            if (policy_opt.is_some()) {
                janus::ShardingPolicySet policy = policy_opt.unwrap();
                rrr::Log_info("Loaded sharding policy version {} with {} tables from RocksDB",
                              policy.version, policy.table_count());
                // Initialize global sharding policy cache
                janus::get_sharding_policy_cache().set_policy(std::move(policy));
            } else {
                rrr::Log_warn("Failed to load sharding policy from RocksDB on reboot");
            }
        } else {
            rrr::Log_info("No sharding policy stored yet - waiting for initializer");
        }
    }

    // Start RPC server
    int port = benchConfig.getConfigPort();
    std::string bind_addr = "0.0.0.0:" + std::to_string(port);

    // @unsafe { RPC thread creation }
    g_config_poll_thread = rusty::Some(rrr::PollThread::create());
    g_config_rpc_server = new rrr::Server(rrr::Server::new_(rusty::Some(g_config_poll_thread.as_ref().unwrap().clone())));

    // @unsafe { RPC service registration - ConfigServiceImpl ownership transferred to server }
    g_config_rpc_server->reg_service_typed(rusty::make_box<janus::ConfigServiceImpl>(*g_config_store));

    // @unsafe { RPC server start }
    if (g_config_rpc_server->start(reinterpret_cast<const int8_t*>(bind_addr.c_str())) != 0) {
        rrr::Log_warn("Failed to start ConfigService RPC server on {}", bind_addr.c_str());
        delete g_config_rpc_server;
        g_config_rpc_server = nullptr;
        g_config_poll_thread = rusty::None;
        return false;
    }

    rrr::Log_info("ConfigService RPC server started on {}", bind_addr.c_str());
    return true;
}

// @unsafe - Network I/O
bool fetch_config_from_cnode() {
    auto& benchConfig = BenchmarkConfig::getInstance();

    const std::string& c_node_addr = benchConfig.getConfigNodeAddr();
    if (c_node_addr.empty()) {
        return false;  // No c-node address specified
    }

    // Don't fetch if we already have config from YAML
    if (benchConfig.getConfig() != nullptr) {
        rrr::Log_info("Already have local config, skipping c-node fetch");
        return true;
    }

    rrr::Log_info("Fetching configuration from c-node at {}", c_node_addr.c_str());

    // @unsafe { network client creation }
    janus::ConfigClient client(c_node_addr);
    client.set_max_retries(5);
    client.set_retry_delay_ms(500);

    // @unsafe { network connect }
    if (!client.connect()) {
        rrr::Log_warn("Failed to connect to c-node at {}", c_node_addr.c_str());
        return false;
    }

    // @unsafe { RPC call }
    auto config_opt = client.fetch_config();
    if (config_opt.is_none()) {
        rrr::Log_warn("Failed to fetch config from c-node");
        client.disconnect();
        return false;
    }

    janus::PersistentConfig persistent = config_opt.unwrap();
    rrr::Log_info("Fetched config version {} from c-node", persistent.version);

    // Convert to transport config
    // @unsafe { memory allocation }
    auto transport_opt = janus::to_transport_config(persistent);
    if (transport_opt.is_none()) {
        rrr::Log_warn("Failed to convert fetched config to transport format");
        client.disconnect();
        return false;
    }

    benchConfig.setConfig(transport_opt.unwrap());
    benchConfig.setNshards(benchConfig.getConfig()->nshards);

    rrr::Log_info("Applied configuration from c-node (nshards={})", benchConfig.getConfig()->nshards);

    // @unsafe { network disconnect }
    client.disconnect();
    return true;
}

// @unsafe - Network I/O
bool fetch_sharding_policy_from_cnode() {
    auto& benchConfig = BenchmarkConfig::getInstance();

    const std::string& c_node_addr = benchConfig.getConfigNodeAddr();
    if (c_node_addr.empty()) {
        rrr::Log_debug("No c-node address specified, skipping sharding policy fetch");
        return false;
    }

    // Check if already initialized
    if (janus::get_sharding_policy_cache().is_initialized()) {
        rrr::Log_info("Sharding policy already initialized, skipping c-node fetch");
        return true;
    }

    rrr::Log_info("Fetching sharding policy from c-node at {}", c_node_addr.c_str());

    // @unsafe { network client creation }
    janus::ConfigClient client(c_node_addr);
    client.set_max_retries(10);  // More retries for policy - it may not be set yet
    client.set_retry_delay_ms(500);

    // @unsafe { network connect }
    if (!client.connect()) {
        rrr::Log_warn("Failed to connect to c-node at {}", c_node_addr.c_str());
        return false;
    }

    // Check if c-node has a sharding policy
    // @unsafe { RPC call }
    auto has_policy_opt = client.has_sharding_policy();
    if (has_policy_opt.is_none()) {
        rrr::Log_warn("Failed to check if c-node has sharding policy");
        client.disconnect();
        return false;
    }

    if (!has_policy_opt.unwrap()) {
        rrr::Log_info("C-node does not have a sharding policy yet - will use fallback routing");
        client.disconnect();
        return true;  // Not an error, just no policy set yet
    }

    // @unsafe { RPC call }
    auto policy_opt = client.fetch_sharding_policy();
    if (policy_opt.is_none()) {
        rrr::Log_warn("Failed to fetch sharding policy from c-node");
        client.disconnect();
        return false;
    }

    janus::ShardingPolicySet policy = policy_opt.unwrap();
    rrr::Log_info("Fetched sharding policy version {} with {} tables from c-node",
                  policy.version, policy.table_count());

    // Initialize global sharding policy cache
    janus::get_sharding_policy_cache().set_policy(std::move(policy));
    rrr::Log_info("Sharding policy cache initialized");

    // @unsafe { network disconnect }
    client.disconnect();
    return true;
}

// @unsafe - Network and RocksDB I/O
void shutdown_config_node() {
    if (g_config_rpc_server != nullptr) {
        rrr::Log_info("Shutting down ConfigService RPC server");
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

}  // namespace mako
