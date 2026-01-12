#pragma once

/**
 * @file config_node_init.h
 * @brief Config node initialization functions.
 *
 * This header provides functions to initialize and manage the configuration node
 * (c-node) functionality. The c-node stores cluster configuration in RocksDB and
 * serves it to other nodes via RPC.
 *
 * Include this header in your startup code to enable config node features.
 */

#include "config_node_settings.h"

namespace janus {

// Forward declarations - avoid including full headers here
class ConfigStore;
class ConfigServiceImpl;

/**
 * @brief Initialize config node components for c-node mode.
 *
 * If this node is configured as a c-node (ConfigNodeSettings::is_config_node()):
 * 1. Opens RocksDB at the configured path
 * 2. On first boot: saves initial config from YAML to RocksDB
 * 3. On reboot: loads config from RocksDB
 * 4. Starts ConfigService RPC server
 *
 * @return true if initialization succeeded or not a c-node, false on error
 */
bool init_config_node();

/**
 * @brief Fetch configuration from a remote c-node.
 *
 * If a c-node address is configured (ConfigNodeSettings::config_node_addr()):
 * 1. Connects to the c-node
 * 2. Fetches the current configuration
 * 3. Applies it to the local Config singleton
 *
 * @return true if config was fetched and applied, false otherwise
 */
bool fetch_config_from_cnode();

/**
 * @brief Shutdown config node components.
 *
 * Gracefully stops the ConfigService RPC server and closes the ConfigStore.
 */
void shutdown_config_node();

}  // namespace janus
