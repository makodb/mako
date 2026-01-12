#pragma once

/**
 * @file config_node_settings.h
 * @brief Simple configuration node settings that can be used from both mako and deptran code.
 *
 * This header intentionally has minimal dependencies to avoid include conflicts.
 * It provides thread-safe singleton access to config node settings.
 */

#include <string>
#include <mutex>

namespace janus {

/**
 * @brief Configuration node settings singleton.
 *
 * This class provides settings for the configuration node feature:
 * - c-node mode: whether this node acts as a config server
 * - c-node address: where to fetch config from (for non-c-nodes)
 * - db path: where c-node stores config in RocksDB
 * - port: RPC port for ConfigService
 */
class ConfigNodeSettings {
public:
    // @safe - singleton access
    static ConfigNodeSettings& instance() {
        static ConfigNodeSettings instance;
        return instance;
    }

    // Getters (thread-safe reads)
    // @safe - const access
    bool is_config_node() const { return is_config_node_; }
    const std::string& config_node_addr() const { return config_node_addr_; }
    const std::string& config_db_path() const { return config_db_path_; }
    int config_port() const { return config_port_; }

    // Setters (thread-safe writes - use mutex since values may be set during startup)
    // @safe - mutex protected
    void set_is_config_node(bool v) {
        std::lock_guard<std::mutex> lock(mutex_);
        is_config_node_ = v;
    }
    void set_config_node_addr(const std::string& addr) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_node_addr_ = addr;
    }
    void set_config_db_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_db_path_ = path;
    }
    void set_config_port(int port) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_port_ = port;
    }

private:
    ConfigNodeSettings() = default;
    ConfigNodeSettings(const ConfigNodeSettings&) = delete;
    ConfigNodeSettings& operator=(const ConfigNodeSettings&) = delete;

    mutable std::mutex mutex_;
    bool is_config_node_{false};
    std::string config_node_addr_;
    std::string config_db_path_{"/tmp/mako_config_db"};
    int config_port_{8888};
};

}  // namespace janus
